/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gflags/gflags.h>
#include <spdlog/fmt/fmt.h>
#include <unistd.h>

#include <any>
#include <boost/asio.hpp>
#include <boost/endian/conversion.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tabulate/table.hpp>
#include <unordered_map>
#include <vector>

#include "bolt/errors.h"
#include "bolt/hydrator.h"
#include "bolt/messages.h"
#include "bolt/pack_stream.h"
#include "bolt/record.h"
#include "bolt/to_string.h"
#include "tools/linenoise/linenoise.h"

DEFINE_string(format, "table", "Output format: table, csv, or json");
DEFINE_string(ip, "127.0.0.1", "RocksGraph server IP address or hostname");
DEFINE_int32(port, 7687, "RocksGraph Bolt port");
DEFINE_string(graph, "default", "Graph name");
DEFINE_string(user, "neo4j", "User name");
DEFINE_string(password, "password", "Password");

namespace {

enum class OutputFormat { kTable, kCsv, kJson };

std::optional<OutputFormat> ParseOutputFormat(std::string_view value) {
  if (value == "table") {
    return OutputFormat::kTable;
  }
  if (value == "csv") {
    return OutputFormat::kCsv;
  }
  if (value == "json") {
    return OutputFormat::kJson;
  }
  return std::nullopt;
}

void Send(boost::asio::ip::tcp::socket& socket, const bolt::PackStream& ps) {
  boost::asio::write(socket, boost::asio::buffer(ps.ConstBuffer()));
}

std::any ReadMessage(boost::asio::ip::tcp::socket& socket,
                     bolt::Hydrator& hydrator) {
  hydrator.ClearErr();
  std::vector<uint8_t> buffer;
  while (true) {
    uint16_t size = 0;
    boost::asio::read(socket, boost::asio::buffer(&size, sizeof(size)));
    boost::endian::big_to_native_inplace(size);
    if (size == 0) {
      if (!buffer.empty()) {
        break;
      }
      continue;
    }
    const auto old_size = buffer.size();
    buffer.resize(old_size + size);
    boost::asio::read(socket,
                      boost::asio::buffer(buffer.data() + old_size, size));
  }

  const auto [message, error] = hydrator.Hydrate(
      {reinterpret_cast<const char*>(buffer.data()), buffer.size()});
  if (error) {
    throw std::runtime_error(
        fmt::format("Failed to parse Bolt message: {}", *error));
  }
  return message;
}

std::string CsvEscape(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(value);
  }
  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      result.push_back('"');
    }
    result.push_back(ch);
  }
  result.push_back('"');
  return result;
}

void PrintCsvRow(const std::vector<std::string>& values) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      std::cout << ',';
    }
    std::cout << CsvEscape(values[i]);
  }
  std::cout << '\n';
}

std::vector<std::string> DisplayValues(const nlohmann::json& values) {
  std::vector<std::string> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    if (value.is_string()) {
      result.push_back(value.get<std::string>());
    } else {
      result.push_back(value.dump());
    }
  }
  return result;
}

bool FetchRecords(boost::asio::ip::tcp::socket& socket,
                  bolt::Hydrator& hydrator, OutputFormat output_format) {
  const auto start = std::chrono::steady_clock::now();
  std::optional<std::vector<std::string>> header;
  std::size_t row_count = 0;
  tabulate::Table table;
  table.format().trim_mode(tabulate::Format::TrimMode::kNone).locale("C");

  while (true) {
    auto message = ReadMessage(socket, hydrator);
    if (message.type() == typeid(std::optional<bolt::Record>)) {
      const auto& record =
          std::any_cast<const std::optional<bolt::Record>&>(message);
      if (!record || !header) {
        throw std::runtime_error("Received a Bolt record before its header");
      }

      nlohmann::json values = nlohmann::json::array();
      for (const auto& item : record->values) {
        values.push_back(bolt::ToJson(item));
      }
      if (values.size() != header->size()) {
        throw std::runtime_error(fmt::format(
            "Mismatched Bolt record: expected {} columns, received {}",
            header->size(), values.size()));
      }

      if (output_format == OutputFormat::kJson) {
        std::cout << values.dump() << '\n';
      } else {
        auto display_values = DisplayValues(values);
        if (output_format == OutputFormat::kTable) {
          table.add_row({display_values.begin(), display_values.end()});
        } else {
          PrintCsvRow(display_values);
        }
      }
      ++row_count;
      continue;
    }

    if (message.type() == typeid(bolt::Success*)) {
      const auto* success = std::any_cast<bolt::Success*>(message);
      if (!header) {
        header = success->fields;
        if (output_format == OutputFormat::kTable) {
          if (!header->empty()) {
            table.add_row({header->begin(), header->end()});
          }
        } else if (output_format == OutputFormat::kCsv) {
          PrintCsvRow(*header);
        } else {
          std::cout << nlohmann::json(*header).dump() << '\n';
        }
        continue;
      }

      if (output_format == OutputFormat::kTable) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
        if (!header->empty()) {
          std::cout << table << '\n';
        }
        std::cout << row_count << " rows (" << elapsed.count() << " ms)\n\n";
      }
      std::cout.flush();
      return true;
    }

    if (message.type() == typeid(std::optional<bolt::Neo4jError>)) {
      const auto& error =
          std::any_cast<const std::optional<bolt::Neo4jError>&>(message);
      if (error) {
        if (error->code.empty()) {
          std::cerr << error->msg << '\n';
        } else {
          std::cerr << error->code << ": " << error->msg << '\n';
        }
      } else {
        std::cerr << "RocksGraph returned an empty Bolt error\n";
      }
      return false;
    }

    if (message.type() == typeid(bolt::Ignored*)) {
      continue;
    }
    throw std::runtime_error(
        fmt::format("Unexpected Bolt message: {}", message.type().name()));
  }
}

bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto left = static_cast<unsigned char>(lhs[i]);
    const auto right = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> ReadStatement() {
  bool is_multiline = false;
  std::string statement;
  while (true) {
    const std::string prompt =
        is_multiline ? "      -> "
                     : fmt::format("RocksGraph[{}]> ", FLAGS_graph);
    std::unique_ptr<char, decltype(&linenoiseFree)> line(
        linenoise(prompt.c_str()), &linenoiseFree);
    if (!line) {
      return std::nullopt;
    }
    if (line.get()[0] == '\0') {
      continue;
    }
    if (!is_multiline && (EqualsIgnoreCase(line.get(), "quit") ||
                          EqualsIgnoreCase(line.get(), "exit"))) {
      return std::nullopt;
    }

    statement.append(line.get());
    if (statement.back() == ';') {
      return statement;
    }
    is_multiline = true;
    statement.push_back(' ');
  }
}

void Completion(const char* /*buffer*/, linenoiseCompletions* /*completions*/) {
}

char* Hints(const char* /*buffer*/, int* /*color*/, int* /*bold*/) {
  return nullptr;
}

void ResetConnection(boost::asio::ip::tcp::socket& socket,
                     bolt::Hydrator& hydrator, bolt::PackStream& ps) {
  ps.Reset();
  ps.AppendReset();
  Send(socket, ps);
  while (true) {
    auto message = ReadMessage(socket, hydrator);
    if (message.type() == typeid(bolt::Success*)) {
      return;
    }
    if (message.type() == typeid(bolt::Ignored*)) {
      continue;
    }
    throw std::runtime_error("Unexpected Bolt message after RESET");
  }
}

void Authenticate(boost::asio::ip::tcp::socket& socket,
                  bolt::Hydrator& hydrator, bolt::PackStream& ps) {
  constexpr uint8_t kHandshake[] = {
      0x60, 0x60, 0xb0, 0x17, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  boost::asio::write(socket, boost::asio::buffer(kHandshake));

  uint8_t accepted_version[4] = {};
  boost::asio::read(socket, boost::asio::buffer(accepted_version));
  if (accepted_version[2] != 4 || accepted_version[3] != 4) {
    throw std::runtime_error(
        fmt::format("Server selected unsupported Bolt version {}.{}",
                    accepted_version[3], accepted_version[2]));
  }

  std::unordered_map<std::string, std::any> metadata = {
      {"scheme", "basic"},
      {"principal", FLAGS_user},
      {"credentials", FLAGS_password},
      {"user_agent", "rocksgraph-cli"},
      {"patch_bolt", std::vector<std::string>{"utc"}},
  };
  ps.Reset();
  ps.AppendHello(metadata);
  Send(socket, ps);

  auto message = ReadMessage(socket, hydrator);
  if (message.type() == typeid(bolt::Success*)) {
    const auto* success = std::any_cast<bolt::Success*>(message);
    if (success->server.find("rocksgraph") == std::string::npos) {
      throw std::runtime_error(
          fmt::format("The server is not RocksGraph: {}", success->server));
    }
    if (success->patches.size() == 1 && success->patches[0] == "utc") {
      hydrator.UseUtc(true);
    }
    return;
  }
  if (message.type() == typeid(std::optional<bolt::Neo4jError>)) {
    const auto& error =
        std::any_cast<const std::optional<bolt::Neo4jError>&>(message);
    throw std::runtime_error(error ? error->msg : "Authentication failed");
  }
  throw std::runtime_error("Unexpected Bolt message during authentication");
}

}  // namespace

int main(int argc, char** argv) {
  gflags::SetUsageMessage(
      "Connect to rg-server and execute semicolon-terminated Cypher "
      "statements.");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const auto output_format = ParseOutputFormat(FLAGS_format);
  if (!output_format) {
    std::cerr << "Invalid --format value '" << FLAGS_format
              << "'; expected table, csv, or json\n";
    return 1;
  }
  if (FLAGS_port < 1 || FLAGS_port > 65535) {
    std::cerr << "Invalid --port value " << FLAGS_port << '\n';
    return 1;
  }

  const bool is_terminal = isatty(STDIN_FILENO) != 0;
  constexpr char kHistoryFile[] = ".rocksgraph_cli_history";
  linenoiseSetCompletionCallback(Completion);
  linenoiseSetHintsCallback(Hints);
  linenoiseSetMultiLine(1);
  if (is_terminal) {
    linenoiseHistoryLoad(kHistoryFile);
  }

  boost::asio::io_context io_context;
  boost::asio::ip::tcp::socket socket(io_context);
  bolt::Hydrator hydrator;
  bolt::PackStream ps;
  try {
    boost::asio::ip::tcp::resolver resolver(io_context);
    boost::asio::connect(
        socket, resolver.resolve(FLAGS_ip, std::to_string(FLAGS_port)));
    Authenticate(socket, hydrator, ps);
  } catch (const std::exception& error) {
    std::cerr << FLAGS_ip << ':' << FLAGS_port << ' ' << error.what() << '\n';
    return 1;
  }

  if (is_terminal) {
    std::cout << "Welcome to the RocksGraph console client. Commands end "
                 "with ';'.\n"
              << "Type 'exit', 'quit' or Ctrl-D to exit.\n\n";
  }

  while (const auto statement = ReadStatement()) {
    try {
      ps.Reset();
      ps.AppendRun(*statement, {}, {{"db", FLAGS_graph}});
      ps.AppendPullN(-1);
      Send(socket, ps);
      const bool succeeded = FetchRecords(socket, hydrator, *output_format);
      if (!succeeded) {
        ResetConnection(socket, hydrator, ps);
      } else if (is_terminal) {
        linenoiseHistoryAdd(statement->c_str());
        linenoiseHistorySave(kHistoryFile);
      }
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }

  try {
    ps.Reset();
    ps.AppendStructMessage(bolt::BoltMsg::Goodbye);
    Send(socket, ps);
  } catch (const std::exception&) {
  }
  return 0;
}
