#pragma once
#include <stdexcept>
#include <string>
#include <vector>
namespace ytwe {
// Translated from yt-dlp b77c9304 _base.py. Regenerate explicitly when
// updating.
struct ClientSpec {
  std::string name, context;
  int id;
  bool javascript, cookies, authentication;
  std::string params;
};
inline const std::vector<ClientSpec> &clients() {
  static const std::vector<ClientSpec> data = {
      {"web",
       R"ctx({"client":{"clientName":"WEB","clientVersion":"2.20260708.00.00","hl":"en"}})ctx",
       1, true, true, false, ""},
      {"web_safari",
       R"ctx({"client":{"clientName":"WEB","clientVersion":"2.20260708.00.00","userAgent":"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.5 Safari/605.1.15,gzip(gfe)","hl":"en"}})ctx",
       1, true, true, false, ""},
      {"web_embedded",
       R"ctx({"client":{"clientName":"WEB_EMBEDDED_PLAYER","clientVersion":"2.20260708.00.00","userAgent":"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.5 Safari/605.1.15,gzip(gfe)","hl":"en"},"thirdParty":{"embedUrl":"https://www.reddit.com/"}})ctx",
       56, true, true, false, ""},
      {"web_music",
       R"ctx({"client":{"clientName":"WEB_REMIX","clientVersion":"1.20260707.12.00","hl":"en"}})ctx",
       67, true, true, false, ""},
      {"web_creator",
       R"ctx({"client":{"clientName":"WEB_CREATOR","clientVersion":"1.20260708.06.00","hl":"en"}})ctx",
       62, true, true, true, ""},
      {"android",
       R"ctx({"client":{"clientName":"ANDROID","clientVersion":"21.26.364","androidSdkVersion":30,"userAgent":"com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip","osName":"Android","osVersion":"11","hl":"en"}})ctx",
       3, false, false, false, ""},
      {"android_vr",
       R"ctx({"client":{"clientName":"ANDROID_VR","clientVersion":"1.65.10","deviceMake":"Oculus","deviceModel":"Quest 3","androidSdkVersion":32,"userAgent":"com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip","osName":"Android","osVersion":"12L","hl":"en"}})ctx",
       28, false, false, false, ""},
      {"ios",
       R"ctx({"client":{"clientName":"IOS","clientVersion":"21.26.4","deviceMake":"Apple","deviceModel":"iPhone16,2","userAgent":"com.google.ios.youtube/21.26.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)","osName":"iPhone","osVersion":"18.3.2.22D82","hl":"en"}})ctx",
       5, false, false, false, ""},
      {"visionos",
       R"ctx({"client":{"clientName":"VISIONOS","clientVersion":"1.02","deviceMake":"Apple","deviceModel":"RealityDevice17,1","userAgent":"Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15","osName":"visionOS","osVersion":"26.5.23O471","hl":"en"}})ctx",
       101, false, false, false, ""},
      {"mweb",
       R"ctx({"client":{"clientName":"MWEB","clientVersion":"2.20260708.05.00","userAgent":"Mozilla/5.0 (iPad; CPU OS 16_7_10 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1,gzip(gfe)","hl":"en"}})ctx",
       2, true, true, false, ""},
      {"tv",
       R"ctx({"client":{"clientName":"TVHTML5","clientVersion":"7.20260707.07.00","userAgent":"Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)","hl":"en"}})ctx",
       7, true, true, false, ""},
      {"tv_downgraded",
       R"ctx({"client":{"clientName":"TVHTML5","clientVersion":"5.20260707","userAgent":"Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version","hl":"en"}})ctx",
       7, true, true, false, ""},
      {"tv_samsung",
       R"ctx({"client":{"clientName":"TVHTML5","clientVersion":"5.20260707","deviceMake":"Samsung","deviceModel":"UKS9800","userAgent":"Mozilla/5.0 (SMART-TV; Linux; Tizen 2.4.0) AppleWebKit/538.1 (KHTML, like Gecko) Version/2.4.0 TV Safari/538.1","osName":"Tizen","osVersion":"2.4.0","hl":"en"}})ctx",
       7, true, true, false, ""},
      {"tv_simply",
       R"ctx({"client":{"clientName":"TVHTML5_SIMPLY","clientVersion":"1.0","hl":"en"}})ctx",
       75, true, false, false, ""}};
  return data;
}
inline const ClientSpec &client_spec(const std::string &name) {
  for (auto &c : clients())
    if (c.name == name)
      return c;
  throw std::runtime_error("unknown player client: " + name);
}
} // namespace ytwe
