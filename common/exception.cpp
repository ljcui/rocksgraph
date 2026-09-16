#include "exception.h"

namespace common {

const char *ErrorCodeToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::UnknownError:
      return "UnknownError";
    case ErrorCode::VertexIdNotFound:
      return "VertexIdNotFound";
    case ErrorCode::StorageEngineError:
      return "StorageEngineError";
    case ErrorCode::EdgeTypeNotFound:
      return "EdgeTypeNotFound";
    case ErrorCode::EdgeIdNotFound:
      return "EdgeIdNotFound";
    case ErrorCode::VertexIndexAlreadyExists:
      return "VertexIndexAlreadyExists";
    case ErrorCode::IndexValueAlreadyExists:
      return "IndexValueAlreadyExists";
    case ErrorCode::NoSuchGraph:
      return "NoSuchGraph";
    case ErrorCode::GraphAlreadyExists:
      return "GraphAlreadyExists";
    case ErrorCode::IndexNotReady:
      return "IndexNotReady";
    case ErrorCode::CypherException:
      return "CypherException";
    case ErrorCode::ParserException:
      return "ParserException";
    case ErrorCode::InputError:
      return "InputError";
    case ErrorCode::EvaluationException:
      return "EvaluationException";
    case ErrorCode::InvalidIndexQuery:
      return "InvalidIndexQuery";
    case ErrorCode::FullTextIndexNotFound:
      return "FullTextIndexNotFound";
    case ErrorCode::VectorIndexNotFound:
      return "VectorIndexNotFound";
    case ErrorCode::VertexUniqueIndexNotFound:
      return "VertexUniqueIndexNotFound";
    case ErrorCode::EdgePropertyIndexAlreadyExists:
      return "EdgePropertyIndexAlreadyExists";
    case ErrorCode::EdgePropertyIndexNotFound:
      return "EdgePropertyIndexNotFound";
    case ErrorCode::BoltDataException:
      return "BoltDataException";
    case ErrorCode::ValueException:
      return "ValueException";
    case ErrorCode::OutOfRange:
      return "OutOfRange";
    case ErrorCode::InvalidParameter:
      return "InvalidParameter";
    case ErrorCode::VectorIndexException:
      return "VectorIndexException";
    case ErrorCode::VertexVectorIndexAlreadyExists:
      return "VertexVectorIndexAlreadyExists";
    case ErrorCode::VertexFullTextIndexAlreadyExists:
      return "VertexFullTextIndexAlreadyExists";
    case ErrorCode::ConnectionDisconnected:
      return "ConnectionDisconnected";
    case ErrorCode::Unimplemented:
      return "Unimplemented";
    case ErrorCode::IOException:
      return "IOException";
    default:
      return "Unknown Error Code";
  }
}

const char *ErrorCodeDesc(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::UnknownError:
      return "Unknown error.";
    case ErrorCode::VertexIdNotFound:
      return "Vertex ID not found.";
    case ErrorCode::StorageEngineError:
      return "Storage engine error.";
    case ErrorCode::EdgeTypeNotFound:
      return "Edge type not found.";
    case ErrorCode::EdgeIdNotFound:
      return "Edge ID not found.";
    case ErrorCode::VertexIndexAlreadyExists:
      return "Vertex index already exists.";
    case ErrorCode::IndexValueAlreadyExists:
      return "Index value already exists.";
    case ErrorCode::NoSuchGraph:
      return "No such graph.";
    case ErrorCode::GraphAlreadyExists:
      return "The graph already exists.";
    case ErrorCode::IndexNotReady:
      return "Index is still building.";
    case ErrorCode::CypherException:
      return "Cypher exception.";
    case ErrorCode::ParserException:
      return "Parser exception.";
    case ErrorCode::InputError:
      return "Input error.";
    case ErrorCode::EvaluationException:
      return "Evaluation exception.";
    case ErrorCode::InvalidIndexQuery:
      return "Invalid index query.";
    case ErrorCode::FullTextIndexNotFound:
      return "Full-text index not found.";
    case ErrorCode::VectorIndexNotFound:
      return "Vector index not found.";
    case ErrorCode::VertexUniqueIndexNotFound:
      return "Vertex unique index not found.";
    case ErrorCode::EdgePropertyIndexAlreadyExists:
      return "Edge property index already exists.";
    case ErrorCode::EdgePropertyIndexNotFound:
      return "Edge property index not found.";
    case ErrorCode::BoltDataException:
      return "Bolt data exception.";
    case ErrorCode::ValueException:
      return "Value exception.";
    case ErrorCode::OutOfRange:
      return "Out of range.";
    case ErrorCode::InvalidParameter:
      return "Invalid parameter.";
    case ErrorCode::VectorIndexException:
      return "Vector index exception.";
    case ErrorCode::VertexVectorIndexAlreadyExists:
      return "Vertex vector index already exists.";
    case ErrorCode::VertexFullTextIndexAlreadyExists:
      return "Vertex full-text index already exists.";
    case ErrorCode::ConnectionDisconnected:
      return "Connection has been disconnected.";
    case ErrorCode::Unimplemented:
      return "Unimplemented.";
    case ErrorCode::IOException:
      return "I/O exception.";
    default:
      return "Unknown Error Code";
  }
}

}  // namespace common
