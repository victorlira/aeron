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

#ifndef AERON_ARCHIVE_CLIENT_H
#define AERON_ARCHIVE_CLIENT_H

#include "aeron_archive_proxy.h"
#include "aeron_archive_control_response_poller.h"
#include "aeron_archive_recording_descriptor_poller.h"
#include "aeron_archive_recording_subscription_descriptor_poller.h"

int aeron_archive_create(
    aeron_archive_t **aeron_archive,
    aeron_archive_context_t *ctx,
    aeron_archive_proxy_t *archive_proxy,
    aeron_subscription_t *subscription,
    aeron_archive_control_response_poller_t *control_response_poller,
    aeron_archive_recording_descriptor_poller_t *recording_descriptor_poller,
    aeron_archive_recording_subscription_descriptor_poller_t *recording_subscription_descriptor_poller,
    int64_t control_session_id,
    int64_t archive_id);

void aeron_archive_idle(aeron_archive_t *aeron_archive);

aeron_archive_control_response_poller_t *aeron_archive_control_response_poller(aeron_archive_t *aeron_archive);

aeron_archive_proxy_t *aeron_archive_proxy(aeron_archive_t *aeron_archive);

int64_t aeron_archive_next_correlation_id(aeron_archive_t *aeron_archive);

#endif //AERON_ARCHIVE_CLIENT_H
