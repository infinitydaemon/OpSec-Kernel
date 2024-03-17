# W.I.P for kernel update. This disregards the apt updates needed to upgrade the kernel releases. include

#include <iostream>
#include <curl/curl.h>
#include <rapidjson/document.h>

const std::string GITHUB_API_URL = "https://api.github.com/repos/infinitydaemon/OpSec-Kernel/releases/latest";
const std::string USER_AGENT = "curl/7.77.0"; 

size_t write_callback(char* ptr, size_t size, size_t nmemb, std::string* data)
{
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

int main()
{
    CURL* curl;
    CURLcode res;
    std::string data;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_URL, GITHUB_API_URL.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

        res = curl_easy_perform(curl);

        if (res == CURLE_OK)
        {
            rapidjson::Document json;
            json.Parse(data.c_str());
            std::string latest_version = json["tag_name"].GetString();
            if (latest_version > CURRENT_VERSION)
            {
                std::cout << "An update is available! Latest version: " << latest_version << std::endl;
            }
            else
            {
                std::cout << "No updates available." << std::endl;
            }
        }
        else
        {
            std::cout << "Curl error: " << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();

    return 0;
}
