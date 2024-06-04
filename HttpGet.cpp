#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include<iostream>
#include<fstream>
#include<sstream>

#include "HttpGet.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage::Streams;
using namespace Windows::Web::Http;

std::wstring HttpGet::Get(const std::wstring& url, const std::filesystem::path& dest, std::wostream &verboseOut)
{
    Windows::Web::Http::HttpClient httpClient;

    auto headers{ httpClient.DefaultRequestHeaders() };
    std::wstring header(L"ie");
    if (!headers.UserAgent().TryParseAdd(header))
    {
        std::wstringstream ss;
        ss << L"Invalid header value: " << header << L" detected while setting up a HTTP Get request header.";
        return ss.str();
    }
    header = L"Mozilla/5.0 (compatible; MSIE 10.0; Windows NT 6.2; WOW64; Trident/6.0)";
    if (!headers.UserAgent().TryParseAdd(header))
    {
        std::wstringstream ss;
        ss << L"Invalid header value: " << header << L" detected while setting up a HTTP Get request header.";
        return ss.str();
    }

    Windows::Web::Http::HttpResponseMessage httpResponseMessage;
    std::wstring httpResponseBody;

    Uri requestUri{ url.c_str() };
    IBuffer bodyBuf;
    try {
        // Send the GET request.
        auto getAsync = httpClient.GetAsync(requestUri);
        getAsync.Progress(
            [&verboseOut](
                IAsyncOperationWithProgress<HttpResponseMessage,
                HttpProgress> const& /* sender */,
                HttpProgress const& args)
            {
                if (args.BytesReceived > 16) {
                    uint64_t siz = args.TotalBytesToReceive.GetUInt64();
                    float progress = siz > 0 ? (float)args.BytesReceived / (float)siz : 0.f;

                    verboseOut <<  L"\rReceived Bytes: " << args.BytesReceived / 1024 << "(KB): " << progress * 100.f << "%              " << std::flush;
                }
            });

        httpResponseMessage = getAsync.get();
        verboseOut << std::endl;

        httpResponseMessage.EnsureSuccessStatusCode();

        bodyBuf = httpResponseMessage.Content().ReadAsBufferAsync().get();
    }
    catch (winrt::hresult_error const& ex) {
        httpResponseBody = ex.message();
        std::wstringstream ss;
        ss << L"Catched an exception during HTTP get request: " << httpResponseBody;
        return ss.str();
    }

     verboseOut << L"Received binary size: " << bodyBuf.Length() << std::endl;
     verboseOut << L"Writing cache file %s\n" << dest.wstring() << std::endl;

    {
        auto parentPath = dest.parent_path();
        if (! std::filesystem::exists(parentPath)) {
            std::filesystem::create_directories(parentPath);
        }

        std::ofstream fs(dest, std::ios::out | std::ios::binary | std::ios::app);
        if (!fs) {
            std::wstringstream ss;
            ss << L"Failed to open file to wirte: \"" << dest.wstring() << "\".";;
            return ss.str();
        }
        fs.write(reinterpret_cast<const char*>(bodyBuf.data()), bodyBuf.Length());
        fs.close();
    }

    return std::wstring();
}
