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
    case ErrorCode::SemanticError:
      return "SemanticError";
    case ErrorCode::ParseError:
      return "ParseError";
    case ErrorCode::InputError:
      return "InputError";
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
    case ErrorCode::BoltDataError:
      return "BoltDataError";
    case ErrorCode::ValueError:
      return "ValueError";
    case ErrorCode::OutOfRange:
      return "OutOfRange";
    case ErrorCode::InvalidParameter:
      return "InvalidParameter";
    case ErrorCode::VectorIndexError:
      return "VectorIndexError";
    case ErrorCode::VertexVectorIndexAlreadyExists:
      return "VertexVectorIndexAlreadyExists";
    case ErrorCode::VertexFullTextIndexAlreadyExists:
      return "VertexFullTextIndexAlreadyExists";
    case ErrorCode::ConnectionDisconnected:
      return "ConnectionDisconnected";
    case ErrorCode::Unimplemented:
      return "Unimplemented";
    case ErrorCode::IOError:
      return "IOError";
    case ErrorCode::InternalError:
      return "InternalError";
    case ErrorCode::QueryCancelled:
      return "QueryCancelled";
    case ErrorCode::MemoryLimitExceeded:
      return "MemoryLimitExceeded";
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
    case ErrorCode::SemanticError:
      return "Semantic error.";
    case ErrorCode::ParseError:
      return "Parse error.";
    case ErrorCode::InputError:
      return "Input error.";
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
    case ErrorCode::BoltDataError:
      return "Bolt data error.";
    case ErrorCode::ValueError:
      return "Value error.";
    case ErrorCode::OutOfRange:
      return "Out of range.";
    case ErrorCode::InvalidParameter:
      return "Invalid parameter.";
    case ErrorCode::VectorIndexError:
      return "Vector index error.";
    case ErrorCode::VertexVectorIndexAlreadyExists:
      return "Vertex vector index already exists.";
    case ErrorCode::VertexFullTextIndexAlreadyExists:
      return "Vertex full-text index already exists.";
    case ErrorCode::ConnectionDisconnected:
      return "Connection has been disconnected.";
    case ErrorCode::Unimplemented:
      return "Unimplemented.";
    case ErrorCode::IOError:
      return "I/O error.";
    case ErrorCode::InternalError:
      return "Internal error.";
    case ErrorCode::QueryCancelled:
      return "Query execution was cancelled.";
    case ErrorCode::MemoryLimitExceeded:
      return "Query memory limit exceeded.";
    default:
      return "Unknown Error Code";
  }
}

}  // namespace common
