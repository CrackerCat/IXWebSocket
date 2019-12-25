/*
 *  ws_cobra_to_sentry.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ixcobra/IXCobraConnection.h>
#include <ixsentry/IXSentryClient.h>
#include <mutex>
#include <queue>
#include <spdlog/spdlog.h>
#include <sstream>
#include <thread>
#include <vector>

namespace ix
{
    int ws_cobra_to_sentry_main(const std::string& appkey,
                                const std::string& endpoint,
                                const std::string& rolename,
                                const std::string& rolesecret,
                                const std::string& channel,
                                const std::string& filter,
                                const std::string& dsn,
                                bool verbose,
                                bool strict,
                                int jobs,
                                const ix::SocketTLSOptions& tlsOptions)
    {
        ix::CobraConnection conn;
        conn.configure(appkey,
                       endpoint,
                       rolename,
                       rolesecret,
                       ix::WebSocketPerMessageDeflateOptions(true),
                       tlsOptions);
        conn.connect();

        Json::FastWriter jsonWriter;
        std::atomic<uint64_t> sentCount(0);
        std::atomic<uint64_t> receivedCount(0);
        std::atomic<bool> errorSending(false);
        std::atomic<bool> stop(false);
        std::atomic<bool> throttled(false);

        std::condition_variable condition;
        std::mutex conditionVariableMutex;
        std::queue<Json::Value> queue;

        auto timer = [&sentCount, &receivedCount] {
            while (true)
            {
                spdlog::info("messages received {} sent {}", receivedCount, sentCount);

                auto duration = std::chrono::seconds(1);
                std::this_thread::sleep_for(duration);
            }
        };

        std::thread t1(timer);

        auto sentrySender = [&condition,
                             &conditionVariableMutex,
                             &queue,
                             verbose,
                             &errorSending,
                             &sentCount,
                             &stop,
                             &throttled,
                             &dsn] {
            SentryClient sentryClient(dsn);

            while (true)
            {
                Json::Value msg;

                {
                    std::unique_lock<std::mutex> lock(conditionVariableMutex);
                    condition.wait(lock, [&queue, &stop] { return !queue.empty() && !stop; });

                    msg = queue.front();
                    queue.pop();
                }

                auto ret = sentryClient.send(msg, verbose);
                HttpResponsePtr response = ret.first;

                if (!response)
                {
                    spdlog::warn("Null HTTP Response");
                    continue;
                }

                if (verbose)
                {
                    for (auto it : response->headers)
                    {
                        spdlog::info("{}: {}", it.first, it.second);
                    }

                    spdlog::info("Upload size: {}", response->uploadSize);
                    spdlog::info("Download size: {}", response->downloadSize);

                    spdlog::info("Status: {}", response->statusCode);
                    if (response->errorCode != HttpErrorCode::Ok)
                    {
                        spdlog::info("error message: {}", response->errorMsg);
                    }

                    if (response->headers["Content-Type"] != "application/octet-stream")
                    {
                        spdlog::info("payload: {}", response->payload);
                    }
                }

                if (response->statusCode != 200)
                {
                    spdlog::error("Error sending data to sentry: {}", response->statusCode);
                    spdlog::error("Body: {}", ret.second);
                    spdlog::error("Response: {}", response->payload);
                    errorSending = true;

                    // Error 429 Too Many Requests
                    if (response->statusCode == 429)
                    {
                        auto retryAfter = response->headers["Retry-After"];
                        std::stringstream ss;
                        ss << retryAfter;
                        int seconds;
                        ss >> seconds;

                        if (!ss.eof() || ss.fail())
                        {
                            seconds = 30;
                            spdlog::warn("Error parsing Retry-After header. "
                                         "Using {} for the sleep duration",
                                         seconds);
                        }

                        spdlog::warn("Error 429 - Too Many Requests. ws will sleep "
                                     "and retry after {} seconds",
                                     retryAfter);

                        throttled = true;
                        auto duration = std::chrono::seconds(seconds);
                        std::this_thread::sleep_for(duration);
                        throttled = false;
                    }
                }
                else
                {
                    ++sentCount;
                }

                if (stop) return;
            }
        };

        // Create a thread pool
        spdlog::info("Starting {} sentry sender jobs", jobs);
        std::vector<std::thread> pool;
        for (int i = 0; i < jobs; i++)
        {
            pool.push_back(std::thread(sentrySender));
        }

        conn.setEventCallback([&conn,
                               &channel,
                               &filter,
                               &jsonWriter,
                               verbose,
                               &throttled,
                               &receivedCount,
                               &condition,
                               &conditionVariableMutex,
                               &queue](ix::CobraConnectionEventType eventType,
                                       const std::string& errMsg,
                                       const ix::WebSocketHttpHeaders& headers,
                                       const std::string& subscriptionId,
                                       CobraConnection::MsgId msgId) {
            if (eventType == ix::CobraConnection_EventType_Open)
            {
                spdlog::info("Subscriber connected");

                for (auto it : headers)
                {
                    spdlog::info("{}: {}", it.first, it.second);
                }
            }
            if (eventType == ix::CobraConnection_EventType_Closed)
            {
                spdlog::info("Subscriber closed");
            }
            else if (eventType == ix::CobraConnection_EventType_Authenticated)
            {
                spdlog::info("Subscriber authenticated");
                conn.subscribe(channel,
                               filter,
                               [&jsonWriter,
                                verbose,
                                &throttled,
                                &receivedCount,
                                &condition,
                                &conditionVariableMutex,
                                &queue](const Json::Value& msg) {
                                   if (verbose)
                                   {
                                       spdlog::info(jsonWriter.write(msg));
                                   }

                                   // If we cannot send to sentry fast enough, drop the message
                                   if (throttled)
                                   {
                                       condition.notify_one();
                                       return;
                                   }

                                   ++receivedCount;

                                   {
                                       std::unique_lock<std::mutex> lock(conditionVariableMutex);
                                       queue.push(msg);
                                   }

                                   condition.notify_one();
                               });
            }
            else if (eventType == ix::CobraConnection_EventType_Subscribed)
            {
                spdlog::info("Subscriber: subscribed to channel {}", subscriptionId);
            }
            else if (eventType == ix::CobraConnection_EventType_UnSubscribed)
            {
                spdlog::info("Subscriber: unsubscribed from channel {}", subscriptionId);
            }
            else if (eventType == ix::CobraConnection_EventType_Error)
            {
                spdlog::error("Subscriber: error {}", errMsg);
            }
            else if (eventType == ix::CobraConnection_EventType_Published)
            {
                spdlog::error("Published message hacked: {}", msgId);
            }
            else if (eventType == ix::CobraConnection_EventType_Pong)
            {
                spdlog::info("Received websocket pong");
            }
        });

        while (true)
        {
            auto duration = std::chrono::seconds(1);
            std::this_thread::sleep_for(duration);

            if (strict && errorSending) break;
        }

        conn.disconnect();

        // join all the bg threads and stop them.
        stop = true;
        for (int i = 0; i < jobs; i++)
        {
            spdlog::error("joining thread {}", i);
            pool[i].join();
        }

        return (strict && errorSending) ? 1 : 0;
    }
} // namespace ix
