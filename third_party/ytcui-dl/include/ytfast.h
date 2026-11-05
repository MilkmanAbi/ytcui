#pragma once
/*
 * ytcui-dl — ytfast.h
 *
 * Public API. Include this single header.
 *
 *   ytfast::CurlGlobalInit init;                  // once at startup
 *   auto results = ytfast::yt_search("lofi", 10);
 *   auto info    = ytfast::yt_get_formats(results[0].id);
 *   auto* audio  = ytfast::InnertubeClient::pick_audio(info.formats);
 *   auto* video  = ytfast::InnertubeClient::pick_video(info.formats, 1080);
 *
 * Link with: -lssl -lcrypto -lz -lpthread
 */
#include "yt_types.h"
#include "yt_http.h"
#include "yt_caps.h"
#include "yt_select.h"
#include "yt_download.h"
#include "yt_stream_dl.h"
#include "yt_innertube.h"
