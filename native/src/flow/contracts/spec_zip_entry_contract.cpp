#include "flow/contracts/spec_zip_entry_contract.hpp"

#include "base/strings.hpp"

namespace di::flow::contracts {

ContractVerdict spec_zip_entry_post(std::size_t result_bytes, std::size_t max_bytes) {
    if (result_bytes <= 64u * 1024 * 1024 && result_bytes <= max_bytes) return std::nullopt;
    return std::string("ZIP output exceeds entry limit or remaining aggregate budget");
}

ContractVerdict spec_zip_entry_post_throw(std::string_view error_message, std::string_view label) {
    if (str::starts_with(error_message, std::string(label) + ": ")) return std::nullopt;
    return std::string("ZIP errors must identify the source");
}

} // namespace di::flow::contracts
