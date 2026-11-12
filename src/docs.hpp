#pragma once

#include <string>
#include <utility>
#include <vector>

namespace loomwright {

/// The full `--docs` guide.
const std::string& language_guide();

/// Built-in API groups used by `--docs TOPIC` and `--list-api`.
const std::vector<std::pair<std::string, std::vector<std::string>>>& api_reference();

std::string render_docs(const std::string& topic);

}  // namespace loomwright
