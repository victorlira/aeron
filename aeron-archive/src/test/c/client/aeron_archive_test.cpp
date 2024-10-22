/*
 * Copyright 2014-2024 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gtest/gtest.h"

#ifdef _MSC_VER
#define AERON_FILE_SEP '\\'
#else
#define AERON_FILE_SEP '/'
#endif

extern "C"
{
#include "client/aeron_archive.h"
#include "aeron_agent.h"
#include "aeron_counter.h"
#include "uri/aeron_uri_string_builder.h"
#include <inttypes.h>
}

#include "../TestArchive.h"
#include "aeron_archive_client/controlResponse.h"

testing::AssertionResult EqualOrErrmsg(const int x, const int y) {
  if (x == y)
    return testing::AssertionSuccess();
  else
    return testing::AssertionFailure() << aeron_errmsg();
}

#define ASSERT_EQ_ERR(__x, __y) ASSERT_TRUE(EqualOrErrmsg((__x), (__y)))

typedef struct fragment_handler_clientd_stct
{
    size_t received;
    int64_t position;
}
fragment_handler_clientd_t;

void fragment_handler(void *clientd, const uint8_t *buffer, size_t length, aeron_header_stct *header)
{
    auto *cd = (fragment_handler_clientd_t *)clientd;
    cd->received++;
    cd->position = aeron_header_position(header);
}

typedef struct credentials_supplier_clientd_stct
{
    aeron_archive_encoded_credentials_t *credentials;
    aeron_archive_encoded_credentials_t *on_challenge_credentials;
}
credentials_supplier_clientd_t;

aeron_archive_encoded_credentials_t default_creds = { "admin:admin", 11 };
aeron_archive_encoded_credentials_t bad_creds = { "admin:NotAdmin", 14 };

credentials_supplier_clientd_t default_creds_clientd = { &default_creds, nullptr };

static aeron_archive_encoded_credentials_t *encoded_credentials_supplier(void *clientd)
{
    return ((credentials_supplier_clientd_t *)clientd)->credentials;
}

static aeron_archive_encoded_credentials_t *encoded_credentials_on_challenge(aeron_archive_encoded_credentials_t *encoded_challenge, void *clientd)
{
    return ((credentials_supplier_clientd_t *)clientd)->on_challenge_credentials;
}

typedef struct recording_signal_consumer_clientd_stct
{
    std::set<std::int32_t> signals;
}
recording_signal_consumer_clientd_t;

void recording_signal_consumer(
    aeron_archive_recording_signal_t *signal,
    void *clientd)
{
    auto cd = (recording_signal_consumer_clientd_t *)clientd;
    cd->signals.insert(signal->recording_signal_code);

    //fprintf(stderr, "RECORDING SIGNAL CODE :: %i\n", signal->recording_signal_code);
}


class AeronCArchiveTestBase
{
public:
    ~AeronCArchiveTestBase()
    {
        if (m_debug)
        {
            std::cout << m_stream.str();
        }
    }

    void DoSetUp(std::int64_t archiveId = 42)
    {
        char aeron_dir[100];
        aeron_default_path(aeron_dir, 100);

        std::string sourceArchiveDir = m_archiveDir + AERON_FILE_SEP + "source";
        m_testArchive = std::make_shared<TestArchive>(
            aeron_dir,
            sourceArchiveDir,
            std::cout,
            "aeron:udp?endpoint=localhost:8010",
            "aeron:udp?endpoint=localhost:0",
            archiveId);
    }

    void DoTearDown()
    {
        if (nullptr != m_archive)
        {
            ASSERT_EQ_ERR(0, aeron_archive_close(m_archive));
        }
        if (nullptr != m_ctx)
        {
            ASSERT_EQ_ERR(0, aeron_archive_context_close(m_ctx));
        }
        if (nullptr != m_dest_archive)
        {
            ASSERT_EQ_ERR(0, aeron_archive_close(m_dest_archive));
        }
        if (nullptr != m_dest_ctx)
        {
            ASSERT_EQ_ERR(0, aeron_archive_context_close(m_dest_ctx));
        }
    }

    void idle()
    {
        aeron_idle_strategy_sleeping_idle((void *)&m_idle_duration_ns, 0);
    }

    void connect(void *recording_signal_consumer_clientd = nullptr)
    {
        ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
        ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
        ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
            m_ctx,
            encoded_credentials_supplier,
            nullptr,
            nullptr,
            &default_creds_clientd));

        if (nullptr != recording_signal_consumer_clientd)
        {
            ASSERT_EQ_ERR(0, aeron_archive_context_set_recording_signal_consumer(
                m_ctx,
                recording_signal_consumer,
                recording_signal_consumer_clientd));
        }

        ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

        m_aeron = aeron_archive_context_get_aeron(m_ctx);
    }

    aeron_subscription_t *addSubscription(std::string channel, int32_t stream_id)
    {
        aeron_async_add_subscription_t *async_add_subscription;
        aeron_subscription_t *subscription = nullptr;

        if (aeron_async_add_subscription(
            &async_add_subscription,
            m_aeron,
            channel.c_str(),
            stream_id,
            nullptr,
            nullptr,
            nullptr,
            nullptr) < 0)
        {
            fprintf(stderr, " -- GOT AN ERROR :: %s\n", aeron_errmsg());
        }

        if (aeron_async_add_subscription_poll(&subscription, async_add_subscription) < 0)
        {
            fprintf(stderr, " -- GOT AN ERROR :: %s\n", aeron_errmsg());
        }
        while (nullptr == subscription)
        {
            idle();
            if (aeron_async_add_subscription_poll(&subscription, async_add_subscription) < 0)
            {
                fprintf(stderr, " -- GOT AN ERROR :: %s\n", aeron_errmsg());
            }
        }

        return subscription;
    }

    aeron_publication_t *addPublication(std::string channel, int32_t stream_id)
    {
        aeron_async_add_publication_t *async_add_publication;
        aeron_publication_t *publication = nullptr;

        if (aeron_async_add_publication(
            &async_add_publication,
            m_aeron,
            channel.c_str(),
            stream_id) < 0)
        {
            fprintf(stderr, " -- GOT AN ERROR :: %s\n", aeron_errmsg());
        }

        if (aeron_async_add_publication_poll(&publication, async_add_publication) < 0)
        {
            fprintf(stderr, " -- GOT AN ERROR :: %s\n", aeron_errmsg());
        }
        while (nullptr == publication)
        {
            idle();
            if (aeron_async_add_publication_poll(&publication, async_add_publication) < 0)
            {
                fprintf(stderr, " -- GOT AN ERROR :: %s\n", aeron_errmsg());
            }
        }

        return publication;
    }

    void setupCounters(int32_t session_id)
    {
        m_counters_reader = aeron_counters_reader(m_aeron);
        m_counter_id = getRecordingCounterId(session_id, m_counters_reader);
        m_recording_id_from_counter = aeron_archive_recording_pos_get_recording_id(m_counters_reader, m_counter_id);
    }

    void waitUntilCaughtUp(int64_t position)
    {
        while (*aeron_counters_reader_addr(m_counters_reader, m_counter_id) < position)
        {
            idle();
        }
    }

    static int32_t getRecordingCounterId(int32_t session_id, aeron_counters_reader_t *counters_reader)
    {
        int32_t counter_id;

        while (AERON_NULL_COUNTER_ID ==
            (counter_id = aeron_archive_recording_pos_find_counter_id_by_session_id(counters_reader, session_id)))
        {
            std::this_thread::yield();
        }

        return counter_id;
    }

    static void offerMessages(
        aeron_publication_t *publication,
        size_t message_count = 10,
        size_t start_count = 0,
        const char *message_prefix = "Message ")
    {
        for (size_t i = 0; i < message_count; i++)
        {
            size_t index = i + start_count;
            char message[1000];
            size_t len = snprintf(message, 1000, "%s%zu", message_prefix, index);

            while (aeron_publication_offer(publication, (uint8_t *)message, len, nullptr, nullptr) < 0)
            {
                aeron_idle_strategy_yielding_idle(nullptr, 0);
            }
        }
    }

    static void offerMessagesToPosition(
        aeron_publication_t *publication,
        int64_t minimum_position,
        const char *message_prefix = "Message ")
    {
        for (size_t i = 0; aeron_publication_position(publication) < minimum_position; i++)
        {
            char message[1000];
            size_t len = snprintf(message, 1000, "%s%zu", message_prefix, i);

            while (aeron_publication_offer(publication, (uint8_t *)message, len, nullptr, nullptr) < 0)
            {
                aeron_idle_strategy_yielding_idle(nullptr, 0);
            }
        }
    }

    static void offerMessages(
        aeron_exclusive_publication_t *exclusive_publication,
        size_t message_count = 10,
        size_t start_count = 0,
        const char *message_prefix = "Message ")
    {
        for (size_t i = 0; i < message_count; i++)
        {
            size_t index = i + start_count;
            char message[1000];
            size_t len = snprintf(message, 1000, "%s%zu", message_prefix, index);

            while (aeron_exclusive_publication_offer(exclusive_publication, (uint8_t *)message, len, nullptr, nullptr) < 0)
            {
                aeron_idle_strategy_yielding_idle(nullptr, 0);
            }
        }
    }

    static void consumeMessages(
        aeron_subscription_t *subscription,
        size_t message_count = 10)
    {
        fragment_handler_clientd_t clientd;
        clientd.received = 0;

        while (clientd.received < message_count)
        {
            if (0 == aeron_subscription_poll(subscription, fragment_handler, (void *)&clientd, 10))
            {
                aeron_idle_strategy_yielding_idle(nullptr, 0);
            }
        }

        ASSERT_EQ(clientd.received, message_count);
    }

    static int64_t consumeMessagesExpectingBound(
        aeron_subscription_t *subscription,
        int64_t bound_position,
        int64_t timeout_ms)
    {
        fragment_handler_clientd_t clientd;
        clientd.received = 0;
        clientd.position = 0;

        int64_t deadline_ms = currentTimeMillis() + timeout_ms;

        while (currentTimeMillis() < deadline_ms)
        {
            if (0 == aeron_subscription_poll(subscription, fragment_handler, (void *)&clientd, 10))
            {
                aeron_idle_strategy_yielding_idle(nullptr, 0);
            }
        }

        return clientd.position;
    }

    bool attemptReplayMerge(
        aeron_archive_replay_merge_t *replay_merge,
        aeron_publication_t *publication,
        aeron_fragment_handler_t handler,
        void *clientd,
        size_t total_message_count,
        size_t *messages_published,
        size_t *received_message_count,
        const char *message_prefix = "Message ")
    {
        for (size_t i = *messages_published; i < total_message_count; i++)
        {
            char message[1000];
            size_t len = snprintf(message, 1000, "%s%zu", message_prefix, i);

            while (aeron_publication_offer(publication, (uint8_t *)message, len, nullptr, nullptr) < 0)
            {
                idle();

                int fragments = aeron_archive_replay_merge_poll(replay_merge, handler, clientd, m_fragment_limit);

                if (0 == fragments && aeron_archive_replay_merge_has_failed(replay_merge))
                {
                    return false;
                }
            }

            (*messages_published)++;
        }

        while (!aeron_archive_replay_merge_is_merged(replay_merge))
        {
            int fragments = aeron_archive_replay_merge_poll(replay_merge, handler, clientd, m_fragment_limit);

            if (0 == fragments && aeron_archive_replay_merge_has_failed(replay_merge))
            {
                return false;
            }

            idle();
        }

        aeron_image_t *image = aeron_archive_replay_merge_image(replay_merge);
        while (*received_message_count < total_message_count)
        {
            int fragments = aeron_image_poll(image, handler, clientd, m_fragment_limit);

            if (0 == fragments && aeron_image_is_closed(image))
            {
                return false;
            }

            idle();
        }

        return true;
    }

    void startDestArchive()
    {
        char aeron_dir[100];
        aeron_default_path(aeron_dir, 100);
        std::string dest_aeron_dir = std::string(aeron_dir) + "_dest";

        const std::string archiveDir = m_archiveDir + AERON_FILE_SEP + "dest";
        const std::string controlChannel = "aeron:udp?endpoint=localhost:8011";
        const std::string replicationChannel = "aeron:udp?endpoint=localhost:8012";

        m_destTestArchive = std::make_shared<TestArchive>(
            dest_aeron_dir, archiveDir, m_stream, controlChannel, replicationChannel, -7777);

        ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_dest_ctx));
        ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_dest_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
        ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
            m_dest_ctx,
            encoded_credentials_supplier,
            nullptr,
            nullptr,
            &default_creds_clientd));
        ASSERT_EQ_ERR(0, aeron_archive_context_set_control_request_channel(m_dest_ctx, controlChannel.c_str()));
    }

    void recordData(
        bool tryStop,
        int64_t *recording_id,
        int64_t *stop_position,
        int64_t *halfway_position,
        size_t message_count = 1000)
    {
        int64_t subscription_id;
        ASSERT_EQ_ERR(0, aeron_archive_start_recording(
            &subscription_id,
            m_archive,
            m_recordingChannel.c_str(),
            m_recordingStreamId,
            AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
            false));

        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        int32_t session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);
        *recording_id = m_recording_id_from_counter;

        bool is_active;
        ASSERT_EQ_ERR(0, aeron_archive_recording_pos_is_active(
            &is_active,
            m_counters_reader,
            m_counter_id,
            m_recording_id_from_counter));
        EXPECT_TRUE(is_active);

        EXPECT_EQ(m_counter_id, aeron_archive_recording_pos_find_counter_id_by_recording_id(
            m_counters_reader,
            m_recording_id_from_counter));

        {
            size_t sib_len = AERON_COUNTER_MAX_LABEL_LENGTH;
            const char source_identity_buffer[AERON_COUNTER_MAX_LABEL_LENGTH] = { '\0' };

            ASSERT_EQ_ERR(0,
                aeron_archive_recording_pos_get_source_identity(
                    m_counters_reader,
                    m_counter_id,
                    source_identity_buffer,
                    &sib_len));
            EXPECT_EQ(9, sib_len);
            EXPECT_STREQ("aeron:ipc", source_identity_buffer);
        }

        int64_t half_count = message_count / 2;

        offerMessages(publication, half_count);
        *halfway_position = aeron_publication_position(publication);
        offerMessages(publication, half_count, half_count);
        consumeMessages(subscription, message_count);

        *stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(*stop_position);

        if (tryStop)
        {
            bool stopped;
            ASSERT_EQ_ERR(0, aeron_archive_try_stop_recording_subscription(
                &stopped,
                m_archive,
                subscription_id));
            EXPECT_TRUE(stopped);
        }
        else
        {
            ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
                m_archive,
                subscription_id));
        }
    }

protected:
    const std::string m_archiveDir = ARCHIVE_DIR;

    const std::string m_recordingChannel = "aeron:udp?endpoint=localhost:3333";
    const std::int32_t m_recordingStreamId = 33;
    const std::string m_replayChannel = "aeron:udp?endpoint=localhost:6666";
    const std::int32_t m_replayStreamId = 66;

    const int m_fragment_limit = 10;

    aeron_counters_reader_t *m_counters_reader;
    std::int32_t m_counter_id;
    std::int64_t m_recording_id_from_counter;

    std::ostringstream m_stream;

    std::shared_ptr<TestArchive> m_testArchive;
    std::shared_ptr<TestArchive> m_destTestArchive;

    bool m_debug = true;

    const uint64_t m_idle_duration_ns = UINT64_C(1000) * UINT64_C(1000); /* 1ms */
    //const uint64_t m_idle_duration_ns = UINT64_C(5) * UINT64_C(1000) * UINT64_C(1000); /* 5ms */

    aeron_archive_context_t *m_ctx = nullptr;
    aeron_archive_t *m_archive = nullptr;
    aeron_t *m_aeron = nullptr;

    aeron_archive_context_t *m_dest_ctx = nullptr;
    aeron_archive_t *m_dest_archive = nullptr;
};

class AeronCArchiveTest : public AeronCArchiveTestBase, public testing::Test
{
public:
    void SetUp() final
    {
        DoSetUp();
    }

    void TearDown() final
    {
        DoTearDown();
    }
};

class AeronCArchiveParamTest : public AeronCArchiveTestBase, public testing::TestWithParam<bool>
{
public:
    void SetUp() final
    {
        DoSetUp();
    }

    void TearDown() final
    {
        DoTearDown();
    }
};

INSTANTIATE_TEST_SUITE_P(AeronCArchive, AeronCArchiveParamTest, testing::Values(true, false));

class AeronCArchiveIdTest : public AeronCArchiveTestBase, public testing::Test
{
};

typedef struct recording_descriptor_consumer_clientd_stct
{
    bool verify_recording_id = false;
    int64_t recording_id;
    bool verify_stream_id = false;
    int32_t stream_id;
    bool verify_start_equals_stop_position = false;
    bool verify_session_id = false;
    int32_t session_id;
    const char *original_channel = nullptr;
    std::set<std::int32_t> session_ids;
    aeron_archive_recording_descriptor_t last_descriptor;
}
recording_descriptor_consumer_clientd_t;

static void recording_descriptor_consumer(
    aeron_archive_recording_descriptor_t *descriptor,
    void *clientd)
{
    auto *cd = (recording_descriptor_consumer_clientd_t *)clientd;

    if (cd->verify_recording_id)
    {
        EXPECT_EQ(cd->recording_id, descriptor->recording_id);
    }
    if (cd->verify_stream_id)
    {
        EXPECT_EQ(cd->stream_id, descriptor->stream_id);
    }
    if (cd->verify_start_equals_stop_position)
    {
        EXPECT_EQ(descriptor->start_position, descriptor->stop_position);
    }
    if (cd->verify_session_id)
    {
        EXPECT_EQ(cd->session_id, descriptor->session_id);
    }
    if (nullptr != cd->original_channel)
    {
        EXPECT_EQ(strlen(cd->original_channel), strlen(descriptor->original_channel));
        EXPECT_STREQ(cd->original_channel, descriptor->original_channel);
    }

    cd->session_ids.insert(descriptor->session_id);

    memcpy(&cd->last_descriptor, descriptor, sizeof(aeron_archive_recording_descriptor_t));
}

struct SubscriptionDescriptor
{
    const std::int64_t m_controlSessionId;
    const std::int64_t m_correlationId;
    const std::int64_t m_subscriptionId;
    const std::int32_t m_streamId;

    SubscriptionDescriptor(
        std::int64_t controlSessionId,
        std::int64_t correlationId,
        std::int64_t subscriptionId,
        std::int32_t streamId) :
        m_controlSessionId(controlSessionId),
        m_correlationId(correlationId),
        m_subscriptionId(subscriptionId),
        m_streamId(streamId)
    {
    }
};

struct subscription_descriptor_consumer_clientd
{
    std::vector<SubscriptionDescriptor> descriptors;
};

static void recording_subscription_descriptor_consumer(
    aeron_archive_recording_subscription_descriptor_t *descriptor,
    void *clientd)
{
    auto cd = (subscription_descriptor_consumer_clientd *)clientd;
    cd->descriptors.emplace_back(
        descriptor->control_session_id,
        descriptor->correlation_id,
        descriptor->subscription_id,
        descriptor->stream_id);
}

TEST_F(AeronCArchiveTest, shouldAsyncConnectToArchive)
{
    aeron_archive_context_t *ctx;
    aeron_archive_async_connect_t *async;
    aeron_archive_t *archive = nullptr;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_async_connect(&async, ctx));

    // the ctx passed into async_connect gets duplicated, so it should be safe to delete it now
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ASSERT_EQ_ERR(0, aeron_archive_async_connect_poll(&archive, async));

    while (nullptr == archive)
    {
        idle();

        ASSERT_NE(-1, aeron_archive_async_connect_poll(&archive, async));
    }

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_TRUE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    ASSERT_EQ(42, aeron_archive_get_archive_id(archive));

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));
}

TEST_F(AeronCArchiveTest, shouldAsyncConnectToArchiveWithPrebuiltAeron)
{
    aeron_archive_context_t *ctx;
    aeron_archive_async_connect_t *async;
    aeron_archive_t *archive = nullptr;

    aeron_context_t *aeron_ctx;
    aeron_t *aeron;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));

    ASSERT_EQ_ERR(0, aeron_context_init(&aeron_ctx));
    ASSERT_EQ_ERR(0, aeron_context_set_dir(aeron_ctx, aeron_archive_context_get_aeron_directory_name(ctx)));
    ASSERT_EQ_ERR(0, aeron_init(&aeron, aeron_ctx));
    ASSERT_EQ_ERR(0, aeron_start(aeron));

    ASSERT_EQ_ERR(0, aeron_archive_context_set_aeron(ctx, aeron));
    ASSERT_EQ_ERR(0, aeron_archive_async_connect(&async, ctx));

    // the ctx passed into async_connect gets duplicated, so it should be safe to delete it now
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ASSERT_EQ_ERR(0, aeron_archive_async_connect_poll(&archive, async));

    while (nullptr == archive)
    {
        idle();

        ASSERT_NE(-1, aeron_archive_async_connect_poll(&archive, async));
    }

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_FALSE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    ASSERT_EQ(42, aeron_archive_get_archive_id(archive));

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));

    ASSERT_EQ_ERR(0, aeron_close(aeron));
    ASSERT_EQ_ERR(0, aeron_context_close(aeron_ctx));
}

TEST_F(AeronCArchiveTest, shouldConnectToArchive)
{
    aeron_archive_context_t *ctx;
    aeron_archive_t *archive = nullptr;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&archive, ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_TRUE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    ASSERT_EQ(42, aeron_archive_get_archive_id(archive));

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));
}

TEST_F(AeronCArchiveTest, shouldConnectToArchiveWithPrebuiltAeron)
{
    aeron_archive_context_t *ctx;
    aeron_archive_t *archive = nullptr;

    aeron_context_t *aeron_ctx;
    aeron_t *aeron;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));

    ASSERT_EQ_ERR(0, aeron_context_init(&aeron_ctx));
    ASSERT_EQ_ERR(0, aeron_context_set_dir(aeron_ctx, aeron_archive_context_get_aeron_directory_name(ctx)));
    ASSERT_EQ_ERR(0, aeron_init(&aeron, aeron_ctx));
    ASSERT_EQ_ERR(0, aeron_start(aeron));

    ASSERT_EQ_ERR(0, aeron_archive_context_set_aeron(ctx, aeron));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&archive, ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_FALSE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    ASSERT_EQ(42, aeron_archive_get_archive_id(archive));

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));

    ASSERT_EQ_ERR(0, aeron_close(aeron));
    ASSERT_EQ_ERR(0, aeron_context_close(aeron_ctx));
}

void invoker_func(void *clientd)
{
    *(bool *)clientd = true;
}

TEST_F(AeronCArchiveTest, shouldConnectToArchiveAndCallInvoker)
{
    aeron_archive_context_t *ctx;
    aeron_archive_t *archive = nullptr;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    bool invokerCalled = false;
    ASSERT_EQ_ERR(0, aeron_archive_context_set_delegating_invoker(
        ctx,
        invoker_func,
        &invokerCalled));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&archive, ctx));
    ASSERT_TRUE(invokerCalled);
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_TRUE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    ASSERT_EQ(42, aeron_archive_get_archive_id(archive));

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));
}

TEST_F(AeronCArchiveTest, shouldObserveErrorOnBadDataOnControlResponseChannel)
{
    aeron_archive_context_t *ctx;
    aeron_archive_t *archive = nullptr;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&archive, ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_TRUE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    char resolved_uri[1000];
    aeron_subscription_try_resolve_channel_endpoint_port(subscription, resolved_uri, 1000);

    m_aeron = aeron_archive_context_get_aeron(ctx);

    aeron_publication_t *publication = addPublication(
        resolved_uri,
        aeron_archive_context_get_control_response_stream_id(ctx));

    while (!aeron_publication_is_connected(publication))
    {
        idle();
    }

    {
        char message[1000];
        size_t len = snprintf(message, 1000, "this will hopefully cause an error");

        while (aeron_publication_offer(publication, (uint8_t *)message, len, nullptr, nullptr) < 0)
        {
            idle();
        }
    }

    while (true)
    {
        int64_t found_start_position;
        ASSERT_EQ_ERR(-1, aeron_archive_get_start_position(
            &found_start_position,
            archive,
            1234)); // <-- should be an invalid recording id

        if (std::string(aeron_errmsg()).find("found schema id") != std::string::npos)
        {
            break;
        }

        std::this_thread::sleep_for(IDLE_SLEEP_MS_5);
    }

    ASSERT_FALSE(std::string(aeron_errmsg()).find("that doesn't match expected") == std::string::npos);

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));
}

typedef struct error_handler_clientd_stct
{
    bool called;
    char message[1000];
}
    error_handler_clientd_t;

void error_handler(void *clientd, int errcode, const char *message)
{
    auto *ehc = (error_handler_clientd_t *)clientd;
    ehc->called = true;
    snprintf(ehc->message, sizeof(ehc->message), "%s", message);
}

TEST_F(AeronCArchiveTest, shouldCallErrorHandlerOnError)
{
    aeron_archive_context_t *ctx;
    aeron_archive_t *archive = nullptr;
    error_handler_clientd_t ehc;

    ehc.called = false;
    ehc.message[0] = '\0';

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_error_handler(ctx, error_handler, &ehc));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&archive, ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_close(ctx));

    ctx = aeron_archive_get_archive_context(archive);
    ASSERT_TRUE(aeron_archive_context_get_owns_aeron_client(ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(archive);
    ASSERT_TRUE(aeron_subscription_is_connected(subscription));

    char resolved_uri[1000];
    aeron_subscription_try_resolve_channel_endpoint_port(subscription, resolved_uri, 1000);

    m_aeron = aeron_archive_context_get_aeron(ctx);

    aeron_publication_t *publication = addPublication(
        resolved_uri,
        aeron_archive_context_get_control_response_stream_id(ctx));

    while (!aeron_publication_is_connected(publication))
    {
        idle();
    }

    {
        struct aeron_archive_client_controlResponse controlResponse;
        struct aeron_archive_client_messageHeader hdr;

        char message[1000];

        aeron_archive_client_controlResponse_wrap_and_apply_header(
            &controlResponse,
            message,
            0,
            1000,
            &hdr);
        aeron_archive_client_controlResponse_set_controlSessionId(&controlResponse, aeron_archive_control_session_id(archive));
        aeron_archive_client_controlResponse_set_correlationId(&controlResponse, 9999999); // this should NOT match
        aeron_archive_client_controlResponse_set_relevantId(&controlResponse, 1234);
        aeron_archive_client_controlResponse_set_code(&controlResponse, aeron_archive_client_controlResponseCode_ERROR);
        const char *err_msg = "fancy error message";
        aeron_archive_client_controlResponse_put_errorMessage(&controlResponse, err_msg, strlen(err_msg) + 1);

        uint64_t len = aeron_archive_client_messageHeader_encoded_length() + aeron_archive_client_controlResponse_encoded_length(&controlResponse);

        // the following prints out the char buffer definition used by the C++ version of this test
        bool printErrorMessageHex = false;
        if (printErrorMessageHex)
        {
            fprintf(stderr, "session id :: %" PRIi64 "\n", aeron_archive_control_session_id(archive));

            uint8_t controlSessionBuffer[100];
            uint64_t sid = aeron_archive_control_session_id(archive);
            memcpy(controlSessionBuffer, &sid, 8);
            for (int i = 0; i < 8; i++)
            {
                fprintf(stderr, "'\\x%02X', ", controlSessionBuffer[i]);
            }
            fprintf(stderr, "\n");

            fprintf(stderr, "len == %" PRIu64 "\n", len);

            fprintf(stderr, "char buffer[] = { ");
            for (uint64_t j = 0; j < len; j++)
            {
                fprintf(stderr, "'\\x%02X', ", (uint8_t)message[j]);
            }
            fprintf(stderr, "};\n");

            for (uint64_t j = 0; j < len; j++)
            {
                fprintf(stderr, "%02X", (uint8_t)message[j]);
            }
            fprintf(stderr, "\n");
        }

        while (aeron_publication_offer(publication, (uint8_t *)message, len, nullptr, nullptr) < 0)
        {
            idle();
        }
    }

    while (true)
    {
        int64_t found_start_position;
        ASSERT_EQ_ERR(-1, aeron_archive_get_start_position(
            &found_start_position,
            archive,
            1234)); // <-- should be an invalid recording id

        if (ehc.called)
        {
            break;
        }

        ASSERT_STREQ("", ehc.message);

        std::this_thread::sleep_for(IDLE_SLEEP_MS_5);
    }

    ASSERT_STREQ("correlation_id=9999999 fancy error message", ehc.message);

    ASSERT_EQ_ERR(0, aeron_archive_close(archive));
}

TEST_F(AeronCArchiveTest, shouldRecordPublicationAndFindRecording)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        int64_t found_stop_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(AERON_NULL_VALUE, found_stop_position);

        int64_t found_max_recorded_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_max_recorded_position(
            &found_max_recorded_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_max_recorded_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_recording_id;
    const char *channel_fragment = "endpoint=localhost:3333";
    ASSERT_EQ_ERR(0, aeron_archive_find_last_matching_recording(
        &found_recording_id,
        m_archive,
        0,
        channel_fragment,
        m_recordingStreamId,
        session_id));

    EXPECT_EQ(m_recording_id_from_counter, found_recording_id);

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    EXPECT_EQ(stop_position, found_stop_position);

    int32_t count;

    recording_descriptor_consumer_clientd_t clientd;
    clientd.verify_recording_id = true;
    clientd.recording_id = found_recording_id;
    clientd.verify_stream_id = true;
    clientd.stream_id = m_recordingStreamId;

    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        found_recording_id,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);
}

TEST_F(AeronCArchiveTest, shouldRecordPublicationAndTryStopById)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        int64_t found_stop_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(AERON_NULL_VALUE, found_stop_position);

        int64_t found_max_recorded_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_max_recorded_position(
            &found_max_recorded_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_max_recorded_position);
    }

    bool stopped;
    EXPECT_EQ(-1, aeron_archive_try_stop_recording_by_identity(
        &stopped,
        m_archive,
        m_recording_id_from_counter + 5)); // invalid recording id

    ASSERT_EQ_ERR(0, aeron_archive_try_stop_recording_by_identity(
        &stopped,
        m_archive,
        m_recording_id_from_counter));
    EXPECT_TRUE(stopped);

    int64_t found_recording_id;
    const char *channel_fragment = "endpoint=localhost:3333";
    ASSERT_EQ_ERR(0, aeron_archive_find_last_matching_recording(
        &found_recording_id,
        m_archive,
        0,
        channel_fragment,
        m_recordingStreamId,
        session_id));

    EXPECT_EQ(m_recording_id_from_counter, found_recording_id);

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    EXPECT_EQ(stop_position, found_stop_position);
}

TEST_F(AeronCArchiveTest, shouldRecordThenReplay)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        bool is_active;
        ASSERT_EQ_ERR(0, aeron_archive_recording_pos_is_active(
            &is_active,
            m_counters_reader,
            m_counter_id,
            m_recording_id_from_counter));
        EXPECT_TRUE(is_active);

        EXPECT_EQ(m_counter_id, aeron_archive_recording_pos_find_counter_id_by_recording_id(
            m_counters_reader,
            m_recording_id_from_counter));

        {
            size_t sib_len = AERON_COUNTER_MAX_LABEL_LENGTH;
            const char source_identity_buffer[AERON_COUNTER_MAX_LABEL_LENGTH] = { '\0' };

            ASSERT_EQ_ERR(0,
                aeron_archive_recording_pos_get_source_identity(
                    m_counters_reader,
                    m_counter_id,
                    source_identity_buffer,
                    &sib_len));
            EXPECT_EQ(9, sib_len);
            EXPECT_STREQ("aeron:ipc", source_identity_buffer);
        }

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    while (found_stop_position != stop_position)
    {
        idle();

        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
    }

    {
        int64_t position = 0;
        int64_t length = stop_position - position;

        aeron_subscription_t *subscription = addSubscription(m_replayChannel, m_replayStreamId);

        aeron_archive_replay_params_t replay_params;
        aeron_archive_replay_params_init(&replay_params);

        replay_params.position = position;
        replay_params.length = length;
        replay_params.file_io_max_length = 4096;

        ASSERT_EQ_ERR(0, aeron_archive_start_replay(
            nullptr,
            m_archive,
            m_recording_id_from_counter,
            m_replayChannel.c_str(),
            m_replayStreamId,
            &replay_params));

        consumeMessages(subscription);

        aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
        ASSERT_EQ(stop_position, aeron_image_position(image));
    }
}

TEST_F(AeronCArchiveTest, shouldRecordThenBoundedReplay)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {

        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    const char *counter_name = "BoundedTestCounter";

    aeron_async_add_counter_t *async_add_counter;
    ASSERT_EQ_ERR(0, aeron_async_add_counter(
        &async_add_counter,
        m_aeron,
        10001,
        (const uint8_t *)counter_name,
        strlen(counter_name),
        counter_name,
        strlen(counter_name)));

    aeron_counter_t *counter = nullptr;
    aeron_async_add_counter_poll(&counter, async_add_counter);
    while (nullptr == counter)
    {
        idle();
        aeron_async_add_counter_poll(&counter, async_add_counter);
    }

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    while (found_stop_position != stop_position)
    {
        idle();

        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
    }

    {
        int64_t position = 0;
        int64_t length = stop_position - position;
        int64_t bounded_length = (length / 4) * 3;
        aeron_counter_set_ordered(aeron_counter_addr(counter), bounded_length);

        aeron_subscription_t *subscription = addSubscription(m_replayChannel, m_replayStreamId);

        aeron_archive_replay_params_t replay_params;
        aeron_archive_replay_params_init(&replay_params);

        replay_params.position = position;
        replay_params.length = length;
        replay_params.bounding_limit_counter_id = counter->counter_id;
        replay_params.file_io_max_length = 4096;

        ASSERT_EQ_ERR(0, aeron_archive_start_replay(
            nullptr,
            m_archive,
            m_recording_id_from_counter,
            m_replayChannel.c_str(),
            m_replayStreamId,
            &replay_params));

        int64_t position_consumed = consumeMessagesExpectingBound(
            subscription, position + bounded_length, 1000);

        EXPECT_LT(position + (length / 2), position_consumed);
        EXPECT_LE(position_consumed, position + bounded_length);
    }
}

TEST_F(AeronCArchiveTest, shouldRecordThenReplayThenTruncate)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        int64_t found_stop_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(AERON_NULL_VALUE, found_stop_position);

        int64_t found_max_recorded_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_max_recorded_position(
            &found_max_recorded_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_max_recorded_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_recording_id;
    const char *channel_fragment = "endpoint=localhost:3333";
    ASSERT_EQ_ERR(0, aeron_archive_find_last_matching_recording(
        &found_recording_id,
        m_archive,
        0,
        channel_fragment,
        m_recordingStreamId,
        session_id));

    EXPECT_EQ(m_recording_id_from_counter, found_recording_id);

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    EXPECT_EQ(stop_position, found_stop_position);

    int64_t position = 0;

    {
        int64_t length = stop_position - position;

        aeron_archive_replay_params_t replay_params;
        aeron_archive_replay_params_init(&replay_params);

        replay_params.position = position;
        replay_params.length = length;
        replay_params.file_io_max_length = 4096;

        aeron_subscription_t *subscription;

        ASSERT_EQ_ERR(0, aeron_archive_replay(
            &subscription,
            m_archive,
            m_recording_id_from_counter,
            m_replayChannel.c_str(),
            m_replayStreamId,
            &replay_params));

        consumeMessages(subscription);

        aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
        EXPECT_EQ(stop_position, aeron_image_position(image));
    }

    ASSERT_EQ_ERR(0, aeron_archive_truncate_recording(
        nullptr,
        m_archive,
        m_recording_id_from_counter,
        position));

    int32_t count;

    recording_descriptor_consumer_clientd_t clientd;
    clientd.verify_start_equals_stop_position = true;

    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        found_recording_id,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);
}

TEST_F(AeronCArchiveTest, shouldRecordAndCancelReplayEarly)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);

        aeron_publication_t *publication;
        ASSERT_EQ_ERR(0, aeron_archive_add_recorded_publication(
            &publication,
            m_archive,
            m_recordingChannel.c_str(),
            m_recordingStreamId));

        {
            aeron_publication_t *duplicate_publication;
            EXPECT_EQ(-1, aeron_archive_add_recorded_publication(
                &duplicate_publication,
                m_archive,
                m_recordingChannel.c_str(),
                m_recordingStreamId));
        }

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        ASSERT_EQ_ERR(0, aeron_archive_stop_recording_publication(m_archive, publication));

        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        while (AERON_NULL_VALUE != found_recording_position)
        {
            idle();

            ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
                &found_recording_position,
                m_archive,
                m_recording_id_from_counter));
        }
    }

    const int64_t position = 0;
    const int64_t length = stop_position - position;
    int64_t replay_session_id;

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;

    ASSERT_EQ_ERR(0, aeron_archive_start_replay(
        &replay_session_id,
        m_archive,
        m_recording_id_from_counter,
        m_replayChannel.c_str(),
        m_replayStreamId,
        &replay_params));

    ASSERT_EQ_ERR(0, aeron_archive_stop_replay(m_archive, replay_session_id));
}

TEST_F(AeronCArchiveTest, shouldRecordAndCancelReplayEarlyWithExclusivePublication)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);

        aeron_exclusive_publication_t *exclusive_publication;
        ASSERT_EQ_ERR(0, aeron_archive_add_recorded_exclusive_publication(
            &exclusive_publication,
            m_archive,
            m_recordingChannel.c_str(),
            m_recordingStreamId));

        aeron_publication_constants_t constants;
        aeron_exclusive_publication_constants(exclusive_publication, &constants);
        session_id = constants.session_id;

        setupCounters(session_id);

        offerMessages(exclusive_publication);
        consumeMessages(subscription);

        stop_position = aeron_exclusive_publication_position(exclusive_publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        ASSERT_EQ_ERR(0, aeron_archive_stop_recording_exclusive_publication(m_archive, exclusive_publication));

        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        while (AERON_NULL_VALUE != found_recording_position)
        {
            idle();

            ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
                &found_recording_position,
                m_archive,
                m_recording_id_from_counter));
        }
    }

    const int64_t position = 0;
    const int64_t length = stop_position - position;
    int64_t replay_session_id;

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;

    ASSERT_EQ_ERR(0, aeron_archive_start_replay(
        &replay_session_id,
        m_archive,
        m_recording_id_from_counter,
        m_replayChannel.c_str(),
        m_replayStreamId,
        &replay_params));

    ASSERT_EQ_ERR(0, aeron_archive_stop_replay(m_archive, replay_session_id));
}

TEST_F(AeronCArchiveTest, shouldGetStartPosition)
{
    int32_t session_id;

    connect();

    aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
    aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);

    offerMessages(publication);
    consumeMessages(subscription);

    int64_t halfway_position = aeron_publication_position(publication);

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    setupCounters(session_id);

    offerMessages(publication);
    consumeMessages(subscription);

    int64_t end_position = aeron_publication_position(publication);

    waitUntilCaughtUp(end_position);

    int64_t found_start_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_start_position(
        &found_start_position,
        m_archive,
        m_recording_id_from_counter));
    ASSERT_EQ(found_start_position, halfway_position);
}

TEST_F(AeronCArchiveTest, shouldReplayRecordingFromLateJoinPosition)
{
    int32_t session_id;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        int64_t current_position = aeron_publication_position(publication);

        waitUntilCaughtUp(current_position);

        aeron_archive_replay_params_t replay_params;
        aeron_archive_replay_params_init(&replay_params);

        replay_params.position = current_position;
        replay_params.file_io_max_length = 4096;

        aeron_subscription_t *replay_subscription;

        ASSERT_EQ_ERR(0, aeron_archive_replay(
            &replay_subscription,
            m_archive,
            m_recording_id_from_counter,
            m_replayChannel.c_str(),
            m_replayStreamId,
            &replay_params));

        offerMessages(publication);
        consumeMessages(subscription);
        consumeMessages(replay_subscription);

        int64_t end_position = aeron_publication_position(publication);

        aeron_image_t *image = aeron_subscription_image_at_index(replay_subscription, 0);
        EXPECT_EQ(end_position, aeron_image_position(image));
    }
}

TEST_F(AeronCArchiveTest, shouldListRegisteredRecordingSubscriptions)
{
    subscription_descriptor_consumer_clientd clientd;

    int32_t expected_stream_id = 7;
    const char *channelOne = "aeron:ipc";
    const char *channelTwo = "aeron:udp?endpoint=localhost:5678";
    const char *channelThree = "aeron:udp?endpoint=localhost:4321";

    connect();

    int64_t subscription_id_one;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id_one,
        m_archive,
        channelOne,
        expected_stream_id,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    int64_t subscription_id_two;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id_two,
        m_archive,
        channelTwo,
        expected_stream_id + 1,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    int64_t subscription_id_three;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id_three,
        m_archive,
        channelThree,
        expected_stream_id + 2,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    int32_t count_one;
    ASSERT_EQ_ERR(0, aeron_archive_list_recording_subscriptions(
        &count_one,
        m_archive,
        0,
        5,
        "ipc",
        expected_stream_id,
        true,
        recording_subscription_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, clientd.descriptors.size());
    EXPECT_EQ(1, count_one);

    clientd.descriptors.clear();

    int32_t count_two;
    ASSERT_EQ_ERR(0, aeron_archive_list_recording_subscriptions(
        &count_two,
        m_archive,
        0,
        5,
        "",
        expected_stream_id,
        false,
        recording_subscription_descriptor_consumer,
        &clientd));
    EXPECT_EQ(3, clientd.descriptors.size());
    EXPECT_EQ(3, count_two);

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id_two));
    clientd.descriptors.clear();

    int32_t count_three;
    ASSERT_EQ_ERR(0, aeron_archive_list_recording_subscriptions(
        &count_three,
        m_archive,
        0,
        5,
        "",
        expected_stream_id,
        false,
        recording_subscription_descriptor_consumer,
        &clientd));
    EXPECT_EQ(2, clientd.descriptors.size());
    EXPECT_EQ(2, count_three);

    /* TODO figure out why this won't compile in CI
    EXPECT_EQ(1, std::count_if(
        clientd.descriptors.begin(),
        clientd.descriptors.end(),
        [=](const SubscriptionDescriptor &descriptor) { return descriptor.m_subscriptionId == subscription_id_one; }));

    EXPECT_EQ(1, std::count_if(
        clientd.descriptors.begin(),
        clientd.descriptors.end(),
        [=](const SubscriptionDescriptor &descriptor) { return descriptor.m_subscriptionId == subscription_id_three; }));
     */
}

TEST_F(AeronCArchiveTest, shouldMergeFromReplayToLive)
{
    const std::size_t termLength = 64 * 1024;
    const std::string message_prefix = "Message ";
    const std::size_t min_messages_per_term = termLength / (message_prefix.length() + AERON_DATA_HEADER_LENGTH);
    const char *control_endpoint = "localhost:23265";
    const char *recording_endpoint = "localhost:23266";
    const char *live_endpoint = "localhost:23267";
    const char *replay_endpoint = "localhost:0";

    char publication_channel[AERON_MAX_PATH + 1];
    char live_destination[AERON_MAX_PATH + 1];
    char replay_destination[AERON_MAX_PATH + 1];
    char recording_channel[AERON_MAX_PATH + 1];
    char subscription_channel[AERON_MAX_PATH + 1];

    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_CONTROL_KEY, control_endpoint);
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_CONTROL_MODE_KEY, AERON_UDP_CHANNEL_CONTROL_MODE_DYNAMIC_VALUE);
        aeron_uri_string_builder_put(&builder, AERON_URI_FC_KEY, "tagged,g:99901/1,t:5s");
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_TERM_LENGTH_KEY, termLength);

        aeron_uri_string_builder_sprint(&builder, publication_channel, AERON_MAX_PATH + 1);
        aeron_uri_string_builder_close(&builder);
    }

    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_ENDPOINT_KEY, live_endpoint);
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_CONTROL_KEY, control_endpoint);

        aeron_uri_string_builder_sprint(&builder, live_destination, AERON_MAX_PATH + 1);
        aeron_uri_string_builder_close(&builder);
    }

    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_ENDPOINT_KEY, replay_endpoint);

        aeron_uri_string_builder_sprint(&builder, replay_destination, AERON_MAX_PATH + 1);
        aeron_uri_string_builder_close(&builder);
    }

    const size_t initial_message_count = min_messages_per_term * 3;
    const size_t subsequent_message_count = min_messages_per_term * 3;
    const size_t total_message_count = min_messages_per_term + subsequent_message_count;

    connect();

    aeron_publication_t *publication = addPublication(publication_channel, m_recordingStreamId);

    int32_t session_id = aeron_publication_session_id(publication);

    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_URI_GTAG_KEY, "99901");
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_SESSION_ID_KEY, session_id);
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_ENDPOINT_KEY, recording_endpoint);
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_CONTROL_KEY, control_endpoint);

        aeron_uri_string_builder_sprint(&builder, recording_channel, AERON_MAX_PATH + 1);
        aeron_uri_string_builder_close(&builder);
    }

    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_CONTROL_MODE_KEY, AERON_UDP_CHANNEL_CONTROL_MODE_MANUAL_VALUE);
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_SESSION_ID_KEY, session_id);

        aeron_uri_string_builder_sprint(&builder, subscription_channel, AERON_MAX_PATH + 1);
        aeron_uri_string_builder_close(&builder);
    }

    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        nullptr,
        m_archive,
        recording_channel,
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_REMOTE,
        true));

    setupCounters(session_id);

    bool is_active;
    ASSERT_EQ_ERR(0, aeron_archive_recording_pos_is_active(
        &is_active,
        m_counters_reader,
        m_counter_id,
        m_recording_id_from_counter));
    EXPECT_TRUE(is_active);

    ASSERT_EQ_ERR(m_counter_id, aeron_archive_recording_pos_find_counter_id_by_recording_id(
        m_counters_reader,
        m_recording_id_from_counter));

    {
        size_t sib_len = AERON_COUNTER_MAX_LABEL_LENGTH;
        const char source_identity_buffer[AERON_COUNTER_MAX_LABEL_LENGTH] = { '\0' };

        ASSERT_EQ_ERR(0,
            aeron_archive_recording_pos_get_source_identity(
                m_counters_reader,
                m_counter_id,
                source_identity_buffer,
                &sib_len));
        EXPECT_STREQ("127.0.0.1:23265", source_identity_buffer);
    }

    offerMessages(publication, initial_message_count);

    waitUntilCaughtUp(aeron_publication_position(publication));

    size_t messages_published = initial_message_count;

    fragment_handler_clientd_t clientd;
    clientd.received = 0;
    clientd.position = 0;

    while (true)
    {
        aeron_subscription_t *subscription = addSubscription(subscription_channel, m_recordingStreamId);

        char replay_channel[AERON_MAX_PATH + 1];
        {
            aeron_uri_string_builder_t builder;

            aeron_uri_string_builder_init_new(&builder);

            aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
            aeron_uri_string_builder_put_int32(&builder, AERON_URI_SESSION_ID_KEY, session_id);

            aeron_uri_string_builder_sprint(&builder, replay_channel, AERON_MAX_PATH + 1);
            aeron_uri_string_builder_close(&builder);
        }

        aeron_archive_replay_merge_t *replay_merge;

        ASSERT_EQ_ERR(0, aeron_archive_replay_merge_init(
            &replay_merge,
            subscription,
            m_archive,
            replay_channel,
            replay_destination,
            live_destination,
            m_recording_id_from_counter,
            clientd.position,
            currentTimeMillis(),
            REPLAY_MERGE_PROGRESS_TIMEOUT_DEFAULT_MS));

        if (attemptReplayMerge(
            replay_merge,
            publication,
            fragment_handler,
            &clientd,
            total_message_count,
            &messages_published,
            &clientd.received))
        {
            ASSERT_EQ_ERR(0, aeron_archive_replay_merge_close(replay_merge));
            break;
        }

        ASSERT_EQ_ERR(0, aeron_archive_replay_merge_close(replay_merge));
        idle();
    }

    EXPECT_EQ(clientd.received, total_message_count);
    EXPECT_EQ(clientd.position, aeron_publication_position(publication));
}

TEST_F(AeronCArchiveTest, shouldFailForIncorrectInitialCredentials)
{
    credentials_supplier_clientd_t bad_creds_clientd = { &bad_creds, nullptr };

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &bad_creds_clientd));

    ASSERT_EQ(-1, aeron_archive_connect(&m_archive, m_ctx));
}

TEST_F(AeronCArchiveTest, shouldBeAbleToHandleBeingChallenged)
{
    aeron_archive_encoded_credentials_t creds = { "admin:adminC", 12 };
    aeron_archive_encoded_credentials_t challenge_creds = { "admin:CSadmin", 13 };
    credentials_supplier_clientd_t creds_clientd = { &creds, &challenge_creds };

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        encoded_credentials_on_challenge,
        nullptr,
        &creds_clientd));

    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));
}

TEST_F(AeronCArchiveTest, shouldExceptionForIncorrectChallengeCredentials)
{
    aeron_archive_encoded_credentials_t creds = { "admin:adminC", 12 };
    aeron_archive_encoded_credentials_t bad_challenge_creds = { "admin:adminNoCS", 15 };
    credentials_supplier_clientd_t creds_clientd = { &creds, &bad_challenge_creds };

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        encoded_credentials_on_challenge,
        nullptr,
        &creds_clientd));

    ASSERT_EQ(-1, aeron_archive_connect(&m_archive, m_ctx));
}

TEST_F(AeronCArchiveTest, shouldPurgeStoppedRecording)
{
    int32_t session_id;
    int64_t stop_position;

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        int64_t found_stop_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(AERON_NULL_VALUE, found_stop_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_recording_id;
    const char *channel_fragment = "endpoint=localhost:3333";
    ASSERT_EQ_ERR(0, aeron_archive_find_last_matching_recording(
        &found_recording_id,
        m_archive,
        0,
        channel_fragment,
        m_recordingStreamId,
        session_id));

    EXPECT_EQ(m_recording_id_from_counter, found_recording_id);

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    EXPECT_EQ(stop_position, found_stop_position);

    int64_t deleted_segments_count;
    ASSERT_EQ_ERR(0, aeron_archive_purge_recording(&deleted_segments_count, m_archive, m_recording_id_from_counter));
    EXPECT_EQ(1, deleted_segments_count);

    int32_t count = 1234; // <-- just to make sure later when it's zero it's because it was explicitly set to 0.

    recording_descriptor_consumer_clientd_t clientd;
    clientd.verify_start_equals_stop_position = true;

    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        found_recording_id,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(0, count);
}

TEST_F(AeronCArchiveTest, shouldReadRecordingDescriptor)
{
    int32_t session_id;

    connect();

    aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    setupCounters(session_id);

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int32_t count = 1234;

    recording_descriptor_consumer_clientd_t clientd;
    clientd.verify_recording_id = true;
    clientd.recording_id = m_recording_id_from_counter;
    clientd.verify_stream_id = true;
    clientd.stream_id = m_recordingStreamId;
    clientd.verify_session_id = true;
    clientd.session_id = session_id;
    clientd.original_channel = m_recordingChannel.c_str();

    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        m_recording_id_from_counter,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);
}

TEST_F(AeronCArchiveTest, shouldFindMultipleRecordingDescriptors)
{
    aeron_publication_t *publication;
    int32_t session_id;
    int64_t subscription_id;
    int64_t subscription_id2;

    std::set<std::int32_t> session_ids;

    connect();

    publication = addPublication(m_recordingChannel, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);
    session_ids.insert(session_id);

    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    setupCounters(session_id);

    const std::string recordingChannel2 = "aeron:udp?endpoint=localhost:3334";
    publication = addPublication(recordingChannel2, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);
    session_ids.insert(session_id);

    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id2,
        m_archive,
        recordingChannel2.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    setupCounters(session_id);

    int32_t count = 1234;

    recording_descriptor_consumer_clientd_t clientd;

    ASSERT_EQ_ERR(0, aeron_archive_list_recordings(
        &count,
        m_archive,
        INT64_MIN,
        10,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(2, count);
    EXPECT_EQ(session_ids, clientd.session_ids);

    ASSERT_EQ_ERR(0, aeron_archive_list_recordings(
        &count,
        m_archive,
        INT64_MIN,
        1,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id2));
}

TEST_F(AeronCArchiveTest, shouldFindRecordingDescriptorForUri)
{
    aeron_publication_t *publication;
    int32_t session_id;
    int64_t subscription_id;
    int64_t subscription_id2;

    std::set<std::int32_t> session_ids;

    connect();

    publication = addPublication(m_recordingChannel, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);
    session_ids.insert(session_id);

    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    setupCounters(session_id);

    const std::string recordingChannel2 = "aeron:udp?endpoint=localhost:3334";
    publication = addPublication(recordingChannel2, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);
    session_ids.insert(session_id);

    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id2,
        m_archive,
        recordingChannel2.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    setupCounters(session_id);

    int32_t count = 1234;

    recording_descriptor_consumer_clientd_t clientd;

    clientd.verify_session_id = true;
    clientd.session_id = session_id;

    ASSERT_EQ_ERR(0, aeron_archive_list_recordings_for_uri(
        &count,
        m_archive,
        INT64_MIN,
        2,
        "3334",
        m_recordingStreamId,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);

    clientd.verify_session_id = false;
    clientd.session_ids.clear();

    ASSERT_EQ_ERR(0, aeron_archive_list_recordings_for_uri(
        &count,
        m_archive,
        INT64_MIN,
        10,
        "333",
        m_recordingStreamId,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(2, count);
    EXPECT_EQ(session_ids, clientd.session_ids);

    ASSERT_EQ_ERR(0, aeron_archive_list_recordings_for_uri(
        &count,
        m_archive,
        INT64_MIN,
        10,
        "no-match",
        m_recordingStreamId,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(0, count);

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id2));
}

TEST_F(AeronCArchiveTest, shouldReadJumboRecordingDescriptor)
{
    int32_t session_id;
    int64_t stop_position;

    std::string recordingChannel = "aeron:udp?endpoint=localhost:3333|term-length=64k|alias=";
    recordingChannel.append(2000, 'X');

    connect();

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        int64_t found_recording_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_recording_position(
            &found_recording_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(stop_position, found_recording_position);

        int64_t found_stop_position;
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));
        EXPECT_EQ(AERON_NULL_VALUE, found_stop_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_stop_position;
    ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
        &found_stop_position,
        m_archive,
        m_recording_id_from_counter));
    EXPECT_EQ(stop_position, found_stop_position);

    int32_t count = 1234;

    recording_descriptor_consumer_clientd_t clientd;
    clientd.verify_recording_id = true;
    clientd.recording_id = m_recording_id_from_counter;
    clientd.verify_stream_id = true;
    clientd.stream_id = m_recordingStreamId;
    clientd.original_channel = recordingChannel.c_str();

    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        m_recording_id_from_counter,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);
}

TEST_F(AeronCArchiveTest, shouldRecordReplicateThenReplay)
{
    int32_t session_id;
    int64_t stop_position;

    startDestArchive();

    recording_signal_consumer_clientd_t rsc_cd;
    rsc_cd.signals.clear();

    ASSERT_EQ_ERR(0, aeron_archive_context_set_recording_signal_consumer(m_dest_ctx, recording_signal_consumer, &rsc_cd));

    connect();

    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_dest_archive, m_dest_ctx));

    ASSERT_EQ(42, aeron_archive_get_archive_id(m_archive));
    ASSERT_EQ(-7777, aeron_archive_get_archive_id(m_dest_archive));

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        bool is_active;
        ASSERT_EQ_ERR(0, aeron_archive_recording_pos_is_active(
            &is_active,
            m_counters_reader,
            m_counter_id,
            m_recording_id_from_counter));
        EXPECT_TRUE(is_active);

        EXPECT_EQ(m_counter_id, aeron_archive_recording_pos_find_counter_id_by_recording_id(
            m_counters_reader,
            m_recording_id_from_counter));

        {
            size_t sib_len = AERON_COUNTER_MAX_LABEL_LENGTH;
            const char source_identity_buffer[AERON_COUNTER_MAX_LABEL_LENGTH] = { '\0' };

            ASSERT_EQ_ERR(0,
                aeron_archive_recording_pos_get_source_identity(
                    m_counters_reader,
                    m_counter_id,
                    source_identity_buffer,
                    &sib_len));
            EXPECT_EQ(9, sib_len);
            EXPECT_STREQ("aeron:ipc", source_identity_buffer);
        }

        offerMessages(publication);
        consumeMessages(subscription);

        stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_stop_position;
    do {
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));

        idle();
    }
    while (found_stop_position != stop_position);

    aeron_archive_replication_params_t replication_params;
    aeron_archive_replication_params_init(&replication_params);

    replication_params.encoded_credentials = &default_creds;

    ASSERT_EQ_ERR(0, aeron_archive_replicate(
        nullptr,
        m_dest_archive,
        m_recording_id_from_counter,
        aeron_archive_context_get_control_request_channel(m_ctx),
        aeron_archive_context_get_control_request_stream_id(m_ctx),
        &replication_params));

    while (0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_SYNC))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_dest_archive);

        idle();
    }

    int64_t position = 0;
    int64_t length = stop_position - position;

    aeron_subscription_t *subscription = addSubscription(m_replayChannel, m_replayStreamId);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;

    ASSERT_EQ_ERR(0, aeron_archive_start_replay(
        nullptr,
        m_dest_archive,
        m_recording_id_from_counter,
        m_replayChannel.c_str(),
        m_replayStreamId,
        &replay_params));

    consumeMessages(subscription);

    aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
    EXPECT_EQ(stop_position, aeron_image_position(image));
}

TEST_P(AeronCArchiveParamTest, shouldRecordReplicateThenStop)
{
    bool tryStop = GetParam();

    int32_t session_id;
    int64_t stop_position;

    startDestArchive();

    recording_signal_consumer_clientd_t rsc_cd;
    rsc_cd.signals.clear();

    ASSERT_EQ_ERR(0, aeron_archive_context_set_recording_signal_consumer(m_dest_ctx, recording_signal_consumer, &rsc_cd));

    connect();

    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_dest_archive, m_dest_ctx));

    ASSERT_EQ(42, aeron_archive_get_archive_id(m_archive));
    ASSERT_EQ(-7777, aeron_archive_get_archive_id(m_dest_archive));

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
    aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

    session_id = aeron_publication_session_id(publication);

    setupCounters(session_id);

    offerMessages(publication);
    consumeMessages(subscription);

    stop_position = aeron_publication_position(publication);

    waitUntilCaughtUp(stop_position);

    aeron_archive_replication_params_t replication_params;
    aeron_archive_replication_params_init(&replication_params);

    replication_params.encoded_credentials = &default_creds;

    int64_t replication_id;
    ASSERT_EQ_ERR(0, aeron_archive_replicate(
        &replication_id,
        m_dest_archive,
        m_recording_id_from_counter,
        aeron_archive_context_get_control_request_channel(m_ctx),
        aeron_archive_context_get_control_request_stream_id(m_ctx),
        &replication_params));

    while (
        0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_REPLICATE) ||
        0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_EXTEND))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_dest_archive);

        idle();
    }

    int64_t position = 0;

    aeron_subscription_t *replay_subscription = addSubscription(m_replayChannel, m_replayStreamId);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.file_io_max_length = 4096;

    ASSERT_EQ_ERR(0, aeron_archive_start_replay(
        nullptr,
        m_dest_archive,
        m_recording_id_from_counter,
        m_replayChannel.c_str(),
        m_replayStreamId,
        &replay_params));

    consumeMessages(replay_subscription);

    if (tryStop)
    {
        bool stopped;
        ASSERT_EQ_ERR(0, aeron_archive_try_stop_replication(&stopped, m_dest_archive, replication_id));
        ASSERT_TRUE(stopped);
    }
    else
    {
        ASSERT_EQ_ERR(0, aeron_archive_stop_replication(m_dest_archive, replication_id));
    }

    offerMessages(publication);

    ASSERT_EQ_ERR(0, consumeMessagesExpectingBound(replay_subscription, 0, 1000));

    while (0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_REPLICATE_END))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_dest_archive);

        idle();
    }

    aeron_image_t *image = aeron_subscription_image_at_index(replay_subscription, 0);
    EXPECT_EQ(stop_position, aeron_image_position(image));
}

TEST_F(AeronCArchiveTest, shouldRecordReplicateTwice)
{
    int32_t session_id;
    int64_t stop_position;

    startDestArchive();

    recording_signal_consumer_clientd_t rsc_cd;
    rsc_cd.signals.clear();

    ASSERT_EQ_ERR(0, aeron_archive_context_set_recording_signal_consumer(m_dest_ctx, recording_signal_consumer, &rsc_cd));

    connect();

    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_dest_archive, m_dest_ctx));

    ASSERT_EQ(42, aeron_archive_get_archive_id(m_archive));
    ASSERT_EQ(-7777, aeron_archive_get_archive_id(m_dest_archive));

    int64_t subscription_id;
    ASSERT_EQ_ERR(0, aeron_archive_start_recording(
        &subscription_id,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId,
        AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
        false));

    int64_t halfway_position;

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(m_recordingChannel, m_recordingStreamId);

        session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        bool is_active;
        ASSERT_EQ_ERR(0, aeron_archive_recording_pos_is_active(
            &is_active,
            m_counters_reader,
            m_counter_id,
            m_recording_id_from_counter));
        EXPECT_TRUE(is_active);

        EXPECT_EQ(m_counter_id, aeron_archive_recording_pos_find_counter_id_by_recording_id(
            m_counters_reader,
            m_recording_id_from_counter));

        {
            size_t sib_len = AERON_COUNTER_MAX_LABEL_LENGTH;
            const char source_identity_buffer[AERON_COUNTER_MAX_LABEL_LENGTH] = { '\0' };

            ASSERT_EQ_ERR(0,
                aeron_archive_recording_pos_get_source_identity(
                    m_counters_reader,
                    m_counter_id,
                    source_identity_buffer,
                    &sib_len));
            EXPECT_EQ(9, sib_len);
            EXPECT_STREQ("aeron:ipc", source_identity_buffer);
        }

        offerMessages(publication);
        consumeMessages(subscription);
        halfway_position = aeron_publication_position(publication);
        waitUntilCaughtUp(halfway_position);

        offerMessages(publication);
        consumeMessages(subscription);
        stop_position = aeron_publication_position(publication);
        waitUntilCaughtUp(stop_position);
    }

    ASSERT_EQ_ERR(0, aeron_archive_stop_recording_subscription(
        m_archive,
        subscription_id));

    int64_t found_stop_position;
    do {
        ASSERT_EQ_ERR(0, aeron_archive_get_stop_position(
            &found_stop_position,
            m_archive,
            m_recording_id_from_counter));

        idle();
    }
    while (found_stop_position != stop_position);

    aeron_archive_replication_params_t replication_params1;
    aeron_archive_replication_params_init(&replication_params1);

    replication_params1.encoded_credentials = &default_creds;
    replication_params1.stop_position = halfway_position;
    replication_params1.replication_session_id = 1;

    ASSERT_EQ_ERR(0, aeron_archive_replicate(
        nullptr,
        m_dest_archive,
        m_recording_id_from_counter,
        aeron_archive_context_get_control_request_channel(m_ctx),
        aeron_archive_context_get_control_request_stream_id(m_ctx),
        &replication_params1));

    while (0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_REPLICATE_END))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_dest_archive);

        idle();
    }

    aeron_archive_replication_params_t replication_params2;
    aeron_archive_replication_params_init(&replication_params2);

    replication_params2.encoded_credentials = &default_creds;
    replication_params2.replication_session_id = 2;

    ASSERT_EQ_ERR(0, aeron_archive_replicate(
        nullptr,
        m_dest_archive,
        m_recording_id_from_counter,
        aeron_archive_context_get_control_request_channel(m_ctx),
        aeron_archive_context_get_control_request_stream_id(m_ctx),
        &replication_params2));

    rsc_cd.signals.clear();

    while (0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_REPLICATE_END))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_dest_archive);

        idle();
    }
}

TEST_F(AeronCArchiveIdTest, shouldResolveArchiveId)
{
    std::int64_t archiveId = 0x4236483BEEF;
    DoSetUp(archiveId);

    connect();

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(m_archive);
    EXPECT_TRUE(aeron_subscription_is_connected(subscription));
    EXPECT_EQ(archiveId, aeron_archive_get_archive_id(m_archive));

    DoTearDown();
}

TEST_F(AeronCArchiveTest, shouldConnectToArchiveWithResponseChannels)
{
    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_control_response_channel(
        m_ctx,
        "aeron:udp?control-mode=response|control=localhost:10002"));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

    aeron_subscription_t *subscription = aeron_archive_get_control_response_subscription(m_archive);
    EXPECT_TRUE(aeron_subscription_is_connected(subscription));
}

TEST_P(AeronCArchiveParamTest, shouldReplayWithResponseChannel)
{
    bool tryStop = GetParam();

    size_t message_count = 1000;
    const char *response_channel = "aeron:udp?control-mode=response|control=localhost:10002";

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_control_response_channel(
        m_ctx,
        response_channel));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

    m_aeron = aeron_archive_context_get_aeron(m_ctx);

    int64_t recording_id, stop_position, halfway_position;

    recordData(tryStop, &recording_id, &stop_position, &halfway_position, message_count);

    int64_t position = 0L;
    int64_t length = stop_position - position;

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;

    aeron_subscription_t *subscription;

    ASSERT_EQ_ERR(0, aeron_archive_replay(
        &subscription,
        m_archive,
        recording_id,
        response_channel,
        m_replayStreamId,
        &replay_params));

    consumeMessages(subscription, message_count);

    aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
    EXPECT_EQ(stop_position, aeron_image_position(image));
}

TEST_P(AeronCArchiveParamTest, shouldBoundedReplayWithResponseChannel)
{
    bool tryStop = GetParam();

    size_t message_count = 1000;
    const char *response_channel = "aeron:udp?control-mode=response|control=localhost:10002";
    const std::int64_t key = 1234567890;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_control_response_channel(
        m_ctx,
        response_channel));
    ASSERT_EQ_ERR(0,
        aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

    m_aeron = aeron_archive_context_get_aeron(m_ctx);

    int64_t recording_id, stop_position, halfway_position;

    recordData(tryStop, &recording_id, &stop_position, &halfway_position, message_count);

    const char *counter_name = "test bounded counter";
    aeron_async_add_counter_t *async_add_counter;
    ASSERT_EQ_ERR(0, aeron_async_add_counter(
        &async_add_counter,
        m_aeron,
        10001,
        (const uint8_t *)&key,
        sizeof(key),
        counter_name,
        strlen(counter_name)));

    aeron_counter_t *counter = nullptr;
    aeron_async_add_counter_poll(&counter, async_add_counter);
    while (nullptr == counter)
    {
        idle();
        aeron_async_add_counter_poll(&counter, async_add_counter);
    }

    aeron_counter_set_ordered(aeron_counter_addr(counter), halfway_position);

    int64_t position = 0L;
    int64_t length = stop_position - position;

    aeron_counter_constants_t counter_constants;
    aeron_counter_constants(counter, &counter_constants);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;
    replay_params.bounding_limit_counter_id = counter_constants.counter_id;

    aeron_subscription_t *subscription;

    ASSERT_EQ_ERR(0, aeron_archive_replay(
        &subscription,
        m_archive,
        recording_id,
        response_channel,
        m_replayStreamId,
        &replay_params));

    consumeMessages(subscription, message_count / 2);

    aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
    EXPECT_EQ(halfway_position, aeron_image_position(image));
}

TEST_P(AeronCArchiveParamTest, shouldStartReplayWithResponseChannel)
{
    bool tryStop = GetParam();

    size_t message_count = 1000;
    const char *response_channel = "aeron:udp?control-mode=response|control=localhost:10003";

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_control_response_channel(
        m_ctx,
        response_channel));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

    m_aeron = aeron_archive_context_get_aeron(m_ctx);

    int64_t recording_id, stop_position, halfway_position;

    recordData(tryStop, &recording_id, &stop_position, &halfway_position, message_count);

    aeron_subscription_t *subscription = addSubscription(response_channel, m_replayStreamId);

    int64_t position = 0L;
    int64_t length = stop_position - position;

    aeron_subscription_constants_t subscription_constants;
    aeron_subscription_constants(subscription, &subscription_constants);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;
    replay_params.subscription_registration_id = subscription_constants.registration_id;

    ASSERT_EQ_ERR(0, aeron_archive_start_replay(
        nullptr,
        m_archive,
        recording_id,
        response_channel,
        m_replayStreamId,
        &replay_params));

    consumeMessages(subscription, message_count);

    aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
    EXPECT_EQ(stop_position, aeron_image_position(image));
}

TEST_P(AeronCArchiveParamTest, shouldStartBoundedReplayWithResponseChannel)
{
    bool tryStop = GetParam();

    size_t message_count = 1000;
    const char *response_channel = "aeron:udp?control-mode=response|control=localhost:10002";
    const std::int64_t key = 1234567890;

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_control_response_channel(
        m_ctx,
        response_channel));
    ASSERT_EQ_ERR(0,
        aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

    m_aeron = aeron_archive_context_get_aeron(m_ctx);

    int64_t recording_id, stop_position, halfway_position;

    recordData(tryStop, &recording_id, &stop_position, &halfway_position, message_count);

    const char *counter_name = "test bounded counter";
    aeron_async_add_counter_t *async_add_counter;
    ASSERT_EQ_ERR(0, aeron_async_add_counter(
        &async_add_counter,
        m_aeron,
        10001,
        (const uint8_t *)&key,
        sizeof(key),
        counter_name,
        strlen(counter_name)));

    aeron_counter_t *counter = nullptr;
    aeron_async_add_counter_poll(&counter, async_add_counter);
    while (nullptr == counter)
    {
        idle();
        aeron_async_add_counter_poll(&counter, async_add_counter);
    }

    aeron_counter_set_ordered(aeron_counter_addr(counter), halfway_position);

    aeron_subscription_t *subscription = addSubscription(response_channel, m_replayStreamId);

    int64_t position = 0L;
    int64_t length = stop_position - position;

    aeron_counter_constants_t counter_constants;
    aeron_counter_constants(counter, &counter_constants);

    aeron_subscription_constants_t subscription_constants;
    aeron_subscription_constants(subscription, &subscription_constants);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = position;
    replay_params.length = length;
    replay_params.file_io_max_length = 4096;
    replay_params.bounding_limit_counter_id = counter_constants.counter_id;
    replay_params.subscription_registration_id = subscription_constants.registration_id;

    ASSERT_EQ_ERR(0, aeron_archive_start_replay(
        nullptr,
        m_archive,
        recording_id,
        response_channel,
        m_replayStreamId,
        &replay_params));

    consumeMessages(subscription, message_count / 2);

    aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
    EXPECT_EQ(halfway_position, aeron_image_position(image));
}

TEST_F(AeronCArchiveTest, shouldDisconnectAfterStopAllReplays)
{
    const char *response_channel = "aeron:udp?control-mode=response|control=localhost:10002";

    ASSERT_EQ_ERR(0, aeron_archive_context_init(&m_ctx));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_control_response_channel(
        m_ctx,
        response_channel));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_idle_strategy(m_ctx, aeron_idle_strategy_sleeping_idle, (void *)&m_idle_duration_ns));
    ASSERT_EQ_ERR(0, aeron_archive_context_set_credentials_supplier(
        m_ctx,
        encoded_credentials_supplier,
        nullptr,
        nullptr,
        &default_creds_clientd));
    ASSERT_EQ_ERR(0, aeron_archive_connect(&m_archive, m_ctx));

    m_aeron = aeron_archive_context_get_aeron(m_ctx);

    addSubscription(m_recordingChannel, m_recordingStreamId);

    aeron_publication_t *publication;
    ASSERT_EQ_ERR(0, aeron_archive_add_recorded_publication(
        &publication,
        m_archive,
        m_recordingChannel.c_str(),
        m_recordingStreamId));

    int32_t session_id = aeron_publication_session_id(publication);

    setupCounters(session_id);

    offerMessages(publication);

    int64_t stop_position = aeron_publication_position(publication);

    waitUntilCaughtUp(stop_position);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = 0L;
    replay_params.file_io_max_length = 4096;

    aeron_subscription_t *subscription;

    ASSERT_EQ_ERR(0, aeron_archive_replay(
        &subscription,
        m_archive,
        m_recording_id_from_counter,
        response_channel,
        m_replayStreamId,
        &replay_params));

    consumeMessages(subscription);

    aeron_image_t *image = aeron_subscription_image_at_index(subscription, 0);
    EXPECT_EQ(stop_position, aeron_image_position(image));

    ASSERT_EQ_ERR(0, aeron_archive_stop_all_replays(
        m_archive,
        m_recording_id_from_counter));

    while (aeron_subscription_is_connected(subscription))
    {
        idle();
    }
}

TEST_P(AeronCArchiveParamTest, shouldRecordAndExtend)
{
    bool tryStop = GetParam();

    connect();

    {
        aeron_subscription_t *subscription = addSubscription(m_recordingChannel, m_recordingStreamId);

        aeron_publication_t *publication;
        ASSERT_EQ_ERR(0, aeron_archive_add_recorded_publication(
            &publication,
            m_archive,
            m_recordingChannel.c_str(),
            m_recordingStreamId));

        int32_t session_id = aeron_publication_session_id(publication);

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        int64_t stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        ASSERT_EQ_ERR(0, aeron_archive_stop_recording_publication(m_archive, publication));

        ASSERT_EQ_ERR(0, aeron_subscription_close(subscription, nullptr, nullptr));
        ASSERT_EQ_ERR(0, aeron_publication_close(publication, nullptr, nullptr));
    }

    recording_descriptor_consumer_clientd_t clientd;

    int32_t count;
    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        m_recording_id_from_counter,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);

    char recordingChannel2[AERON_MAX_PATH];

    aeron_uri_string_builder_t builder;
    ASSERT_EQ_ERR(0, aeron_uri_string_builder_init_on_string(&builder, "aeron:udp?endpoint=localhost:3332"));
    ASSERT_EQ_ERR(0, aeron_uri_string_builder_set_initial_position(
        &builder,
        clientd.last_descriptor.stop_position,
        clientd.last_descriptor.initial_term_id,
        clientd.last_descriptor.term_buffer_length));
    ASSERT_EQ_ERR(0, aeron_uri_string_builder_sprint(&builder, recordingChannel2, sizeof(recordingChannel2)));
    ASSERT_EQ_ERR(0, aeron_uri_string_builder_close(&builder));

    {
        aeron_subscription_t *subscription = addSubscription(recordingChannel2, m_recordingStreamId);
        aeron_publication_t *publication = addPublication(recordingChannel2, m_recordingStreamId);

        int32_t session_id = aeron_publication_session_id(publication);

        int64_t subscription_id;
        ASSERT_EQ_ERR(0, aeron_archive_extend_recording(
            &subscription_id,
            m_archive,
            m_recording_id_from_counter,
            recordingChannel2,
            m_recordingStreamId,
            AERON_ARCHIVE_SOURCE_LOCATION_LOCAL,
            false));

        setupCounters(session_id);

        offerMessages(publication);
        consumeMessages(subscription);

        int64_t stop_position = aeron_publication_position(publication);

        waitUntilCaughtUp(stop_position);

        if (tryStop)
        {
            bool stopped;
            ASSERT_EQ_ERR(0, aeron_archive_try_stop_recording_channel_and_stream(&stopped, m_archive, recordingChannel2, m_recordingStreamId));
            EXPECT_TRUE(stopped);
        }
        else
        {
            ASSERT_EQ_ERR(0, aeron_archive_stop_recording_channel_and_stream(m_archive, recordingChannel2, m_recordingStreamId));
        }

        ASSERT_EQ_ERR(0, aeron_subscription_close(subscription, nullptr, nullptr));
        ASSERT_EQ_ERR(0, aeron_publication_close(publication, nullptr, nullptr));
    }

    ASSERT_EQ_ERR(0, aeron_archive_list_recording(
        &count,
        m_archive,
        m_recording_id_from_counter,
        recording_descriptor_consumer,
        &clientd));
    EXPECT_EQ(1, count);

    aeron_archive_replay_params_t replay_params;
    aeron_archive_replay_params_init(&replay_params);

    replay_params.position = clientd.last_descriptor.start_position;
    replay_params.file_io_max_length = 4096;

    aeron_subscription_t *replay_subscription;

    ASSERT_EQ_ERR(0, aeron_archive_replay(
        &replay_subscription,
        m_archive,
        m_recording_id_from_counter,
        m_replayChannel.c_str(),
        m_replayStreamId,
        &replay_params));

    consumeMessages(replay_subscription, 20);

    aeron_image_t *image = aeron_subscription_image_at_index(replay_subscription, 0);
    ASSERT_EQ(clientd.last_descriptor.stop_position, aeron_image_position(image));
}

#define TERM_LENGTH AERON_LOGBUFFER_TERM_MIN_LENGTH
#define SEGMENT_LENGTH (TERM_LENGTH * 2)
#define MTU_LENGTH 1024

TEST_F(AeronCArchiveTest, shouldPurgeSegments)
{
    int32_t session_id;
    int64_t stop_position;

    recording_signal_consumer_clientd_t rsc_cd;

    connect(&rsc_cd);

    char publication_channel[AERON_MAX_PATH];
    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_ENDPOINT_KEY, "localhost:3333");
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_TERM_LENGTH_KEY, TERM_LENGTH);
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_MTU_LENGTH_KEY, MTU_LENGTH);

        aeron_uri_string_builder_sprint(&builder, publication_channel, AERON_MAX_PATH);
        aeron_uri_string_builder_close(&builder);
    }

    aeron_publication_t *publication;
    ASSERT_EQ_ERR(0, aeron_archive_add_recorded_publication(
        &publication,
        m_archive,
        publication_channel,
        m_recordingStreamId));

    session_id = aeron_publication_session_id(publication);

    setupCounters(session_id);

    int64_t targetPosition = (SEGMENT_LENGTH * 3L) + 1;
    offerMessagesToPosition(publication, targetPosition);

    stop_position = aeron_publication_position(publication);

    waitUntilCaughtUp(stop_position);

    int64_t start_position = 0;
    int64_t segment_file_base_position = aeron_archive_segment_file_base_position(
        start_position,
        SEGMENT_LENGTH * 2L,
        TERM_LENGTH,
        SEGMENT_LENGTH);

    int64_t count;
    ASSERT_EQ_ERR(0, aeron_archive_purge_segments(
        &count,
        m_archive,
        m_recording_id_from_counter,
        segment_file_base_position));

    while (0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_DELETE))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_archive);

        idle();
    }

    ASSERT_EQ(2, count);

    ASSERT_EQ_ERR(0, aeron_archive_get_start_position(&start_position, m_archive, m_recording_id_from_counter));
    ASSERT_EQ(start_position, segment_file_base_position);
}

TEST_F(AeronCArchiveTest, shouldDetachAndDeleteSegments)
{
    int32_t session_id;
    int64_t stop_position;

    recording_signal_consumer_clientd_t rsc_cd;

    connect(&rsc_cd);

    char publication_channel[AERON_MAX_PATH];
    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_ENDPOINT_KEY, "localhost:3333");
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_TERM_LENGTH_KEY, TERM_LENGTH);
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_MTU_LENGTH_KEY, MTU_LENGTH);

        aeron_uri_string_builder_sprint(&builder, publication_channel, AERON_MAX_PATH);
        aeron_uri_string_builder_close(&builder);
    }

    aeron_publication_t *publication;
    ASSERT_EQ_ERR(0, aeron_archive_add_recorded_publication(
        &publication,
        m_archive,
        publication_channel,
        m_recordingStreamId));

    session_id = aeron_publication_session_id(publication);

    setupCounters(session_id);

    int64_t targetPosition = (SEGMENT_LENGTH * 4L) + 1;
    offerMessagesToPosition(publication, targetPosition);

    stop_position = aeron_publication_position(publication);

    waitUntilCaughtUp(stop_position);

    int64_t start_position = 0;
    int64_t segment_file_base_position = aeron_archive_segment_file_base_position(
        start_position,
        SEGMENT_LENGTH * 3L,
        TERM_LENGTH,
        SEGMENT_LENGTH);

    ASSERT_EQ_ERR(0, aeron_archive_detach_segments(
        m_archive,
        m_recording_id_from_counter,
        segment_file_base_position));

    int64_t count;
    ASSERT_EQ_ERR(0, aeron_archive_delete_detached_segments(
        &count,
        m_archive,
        m_recording_id_from_counter));

    while (0 == rsc_cd.signals.count(AERON_ARCHIVE_CLIENT_RECORDING_SIGNAL_DELETE))
    {
        aeron_archive_poll_for_recording_signals(nullptr, m_archive);

        idle();
    }

    ASSERT_EQ(3, count);

    ASSERT_EQ_ERR(0, aeron_archive_get_start_position(&start_position, m_archive, m_recording_id_from_counter));
    ASSERT_EQ(start_position, segment_file_base_position);
}

TEST_F(AeronCArchiveTest, shouldDetachAndReattachSegments)
{
    int32_t session_id;
    int64_t stop_position;

    recording_signal_consumer_clientd_t rsc_cd;

    connect(&rsc_cd);

    char publication_channel[AERON_MAX_PATH];
    {
        aeron_uri_string_builder_t builder;

        aeron_uri_string_builder_init_new(&builder);

        aeron_uri_string_builder_put(&builder, AERON_URI_STRING_BUILDER_MEDIA_KEY, "udp");
        aeron_uri_string_builder_put(&builder, AERON_UDP_CHANNEL_ENDPOINT_KEY, "localhost:3333");
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_TERM_LENGTH_KEY, TERM_LENGTH);
        aeron_uri_string_builder_put_int32(&builder, AERON_URI_MTU_LENGTH_KEY, MTU_LENGTH);

        aeron_uri_string_builder_sprint(&builder, publication_channel, AERON_MAX_PATH);
        aeron_uri_string_builder_close(&builder);
    }

    aeron_publication_t *publication;
    ASSERT_EQ_ERR(0, aeron_archive_add_recorded_publication(
        &publication,
        m_archive,
        publication_channel,
        m_recordingStreamId));

    session_id = aeron_publication_session_id(publication);

    setupCounters(session_id);

    int64_t targetPosition = (SEGMENT_LENGTH * 5L) + 1;
    offerMessagesToPosition(publication, targetPosition);

    stop_position = aeron_publication_position(publication);

    waitUntilCaughtUp(stop_position);

    int64_t start_position = 0;
    int64_t segment_file_base_position = aeron_archive_segment_file_base_position(
        start_position,
        SEGMENT_LENGTH * 4L,
        TERM_LENGTH,
        SEGMENT_LENGTH);

    ASSERT_EQ_ERR(0, aeron_archive_detach_segments(
        m_archive,
        m_recording_id_from_counter,
        segment_file_base_position));

    ASSERT_EQ_ERR(0, aeron_archive_get_start_position(&start_position, m_archive, m_recording_id_from_counter));
    ASSERT_EQ(start_position, segment_file_base_position);

    int64_t count;
    ASSERT_EQ_ERR(0, aeron_archive_attach_segments(
        &count,
        m_archive,
        m_recording_id_from_counter));

    ASSERT_EQ(4, count);

    ASSERT_EQ_ERR(0, aeron_archive_get_start_position(&start_position, m_archive, m_recording_id_from_counter));
    ASSERT_EQ(start_position, 0);
}
