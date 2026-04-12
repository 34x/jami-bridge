#pragma once
/// @file api_docs.h
/// @brief Inline HTML and OpenAPI specs for the API documentation page.

#include <string>

namespace jami {
namespace docs {

const std::string& index_html();
const std::string& openapi_json();

} // namespace docs
} // namespace jami