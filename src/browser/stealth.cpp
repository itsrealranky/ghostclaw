#include "ghostclaw/browser/stealth.hpp"

namespace ghostclaw::browser {

const std::string &StealthManager::stealth_script() {
  static const std::string script = R"js(
// Stealth patches — runs before any page script
(function(){
  // 1. navigator.webdriver = false
  Object.defineProperty(navigator, 'webdriver', {get: () => false});

  // 2. Patch chrome.runtime to appear as normal extension runtime
  if (!window.chrome) { window.chrome = {}; }
  if (!window.chrome.runtime) {
    window.chrome.runtime = {
      connect: function(){},
      sendMessage: function(){}
    };
  }

  // 3. Patch navigator.plugins to have realistic length
  Object.defineProperty(navigator, 'plugins', {
    get: () => {
      const arr = [
        {name:'Chrome PDF Plugin',filename:'internal-pdf-viewer',description:'Portable Document Format'},
        {name:'Chrome PDF Viewer',filename:'mhjfbmdgcfjbbpaeojofohoefgiehjai',description:''},
        {name:'Native Client',filename:'internal-nacl-plugin',description:''}
      ];
      arr.item = function(i){ return this[i] || null; };
      arr.namedItem = function(n){ return this.find(p=>p.name===n) || null; };
      arr.refresh = function(){};
      return arr;
    }
  });

  // 4. Patch navigator.languages
  Object.defineProperty(navigator, 'languages', {
    get: () => ['en-US', 'en']
  });

  // 5. Patch permissions.query for notifications
  const origQuery = window.Permissions && Permissions.prototype.query;
  if (origQuery) {
    Permissions.prototype.query = function(parameters) {
      if (parameters.name === 'notifications') {
        return Promise.resolve({state: Notification.permission});
      }
      return origQuery.call(this, parameters);
    };
  }

  // 6. Conceal automation-related window properties
  delete window.cdc_adoQpoasnfa76pfcZLmcfl_Array;
  delete window.cdc_adoQpoasnfa76pfcZLmcfl_Promise;
  delete window.cdc_adoQpoasnfa76pfcZLmcfl_Symbol;
})();
)js";
  return script;
}

common::Status StealthManager::enable(CDPClient &client) {
  auto result = client.send_command(
      "Page.addScriptToEvaluateOnNewDocument",
      {{"source", stealth_script()}});
  if (!result.ok()) {
    return common::Status::error("stealth injection failed: " + result.error());
  }
  return common::Status::success();
}

} // namespace ghostclaw::browser
