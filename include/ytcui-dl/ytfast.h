#pragma once
/*
 * ytcui-dl — ytfast.h
 *
 * Public API. Include this single header in your project.
 *
 * Quick start:
 *   #include "ytfast.h"
 *
 *   // Global init (once at startup)
 *   ytfast::CurlGlobalInit curl_init;
 *
 *   // Use the singleton (shared visitor_data + URL cache across all callers)
 *   auto results = ytfast::yt_search("lofi hip hop", 10);
 *   std::string audio = ytfast::yt_best_audio(results[0].id);
 *   std::string video = ytfast::yt_best_video(results[0].id, 1080);
 *
 *   // Or use the client directly for more control:
 *   auto& yt = ytfast::InnertubeClient::get_instance();
 *   yt.prefetch(results[1].id);  // warm cache in background
 *   auto info = yt.get_stream_formats("dQw4w9WgXcQ");
 *
 * Link with: -lcurl -lssl -lcrypto -lpthread
 */

#include "ytcui-dl/ytfast_types.h"
#include "ytcui-dl/ytfast_http.h"
#include "ytcui-dl/ytfast_innertube.h"
