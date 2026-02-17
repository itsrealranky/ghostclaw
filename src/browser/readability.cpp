#include "ghostclaw/browser/readability.hpp"

#include "ghostclaw/common/json_util.hpp"

namespace ghostclaw::browser {

const std::string &ReadabilityExtractor::extraction_script() {
  static const std::string script = R"js(
(function(){
  // Remove noise elements
  var selectors = ['nav','footer','header','aside','.cookie-banner',
    '.cookie-consent','[role="banner"]','[role="navigation"]',
    '[role="contentinfo"]','.ad','.ads','.advertisement'];
  // Find main content container
  var main = document.querySelector('article') ||
             document.querySelector('[role="main"]') ||
             document.querySelector('main') ||
             document.body;
  if (!main) return '';

  // Clone to avoid modifying the live DOM
  var clone = main.cloneNode(true);

  // Remove noise from clone
  selectors.forEach(function(sel){
    var els = clone.querySelectorAll(sel);
    for(var i=0;i<els.length;i++) els[i].remove();
  });

  // Also remove script and style tags
  var removeTags = clone.querySelectorAll('script,style,noscript,iframe');
  for(var i=0;i<removeTags.length;i++) removeTags[i].remove();

  // Get text content and clean whitespace
  var text = clone.innerText || clone.textContent || '';
  // Collapse multiple newlines and trim
  text = text.replace(/\n{3,}/g, '\n\n').replace(/[ \t]+/g, ' ').trim();
  return text;
})()
)js";
  return script;
}

common::Result<std::string> ReadabilityExtractor::extract(CDPClient &client) {
  auto result = client.evaluate_js(extraction_script());
  if (!result.ok()) {
    return common::Result<std::string>::failure(
        "readability extraction failed: " + result.error());
  }

  // The evaluate_js returns a JsonMap with "result" containing the value
  auto result_it = result.value().find("result");
  if (result_it != result.value().end()) {
    // The result field is a nested JSON object like {"type":"string","value":"..."}
    std::string value = common::json_get_string(result_it->second, "value");
    if (!value.empty()) {
      return common::Result<std::string>::success(std::move(value));
    }
    // If the value extraction fails, the raw result might be the string itself
    return common::Result<std::string>::success(result_it->second);
  }

  // Try extracting the value directly
  auto value_it = result.value().find("value");
  if (value_it != result.value().end()) {
    return common::Result<std::string>::success(value_it->second);
  }

  return common::Result<std::string>::success("");
}

} // namespace ghostclaw::browser
