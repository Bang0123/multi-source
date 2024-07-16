/*
 * Copyright (c) 2024 Demant A/S (modified to send pre-encoded LC3)
 * Copyright (c) 2022-2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/audio/bap_lc3_preset.h>

BUILD_ASSERT(strlen(CONFIG_BROADCAST_CODE) <= BT_AUDIO_BROADCAST_CODE_SIZE,
	     "Invalid broadcast code");

/* Zephyr Controller works best while Extended Advertising interval to be a multiple
 * of the ISO Interval minus 10 ms (max. advertising random delay). This is
 * required to place the AUX_ADV_IND PDUs in a non-overlapping interval with the
 * Broadcast ISO radio events.
 *
 * I.e. for a 7.5 ms ISO interval use 90 ms minus 10 ms ==> 80 ms advertising
 * interval.
 * And, for 10 ms ISO interval, can use 90 ms minus 10 ms ==> 80 ms advertising
 * interval.
 */
#define BT_LE_EXT_ADV_CUSTOM                                                                       \
	BT_LE_ADV_PARAM(BT_LE_ADV_OPT_EXT_ADV, 0x0080, 0x0080, NULL)

/* When BROADCAST_ENQUEUE_COUNT > 1 we can enqueue enough buffers to ensure that
 * the controller is never idle
 */
#define BROADCAST_ENQUEUE_COUNT 3U
#define TOTAL_BUF_NEEDED (BROADCAST_ENQUEUE_COUNT * CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT)

BUILD_ASSERT(CONFIG_BT_ISO_TX_BUF_COUNT >= TOTAL_BUF_NEEDED,
	     "CONFIG_BT_ISO_TX_BUF_COUNT should be at least "
	     "BROADCAST_ENQUEUE_COUNT * CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT");

#if defined(CONFIG_BAP_BROADCAST_16_2_1)

static struct bt_bap_lc3_preset preset_active = BT_BAP_LC3_BROADCAST_PRESET_16_2_1(
	BT_AUDIO_LOCATION_FRONT_LEFT,
	BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED);

#define BT_AUDIO_BROADCAST_NAME "16Khz Stereo"

#define SAMPLES_PER_FRAME	160

static const uint8_t lc3_left[] = {
#include "Sine1000Hz_1s_16.lc3.inc"
};

static const uint8_t lc3_right[] = {
#include "Sine1500Hz_1s_16.lc3.inc"
};

#elif defined(CONFIG_BAP_BROADCAST_24_2_1)

static struct bt_bap_lc3_preset preset_active = BT_BAP_LC3_BROADCAST_PRESET_24_2_1(
	BT_AUDIO_LOCATION_FRONT_LEFT,
	BT_AUDIO_CONTEXT_TYPE_UNSPECIFIED);

#define BT_AUDIO_BROADCAST_NAME "24Khz Stereo"

#define SAMPLES_PER_FRAME	240

static const uint8_t lc3_left[] = {
#include "Sine1000Hz_1s_24.lc3.inc"
};

static const uint8_t lc3_right[] = {
#include "Sine1500Hz_1s_24.lc3.inc"
};

#endif

static struct broadcast_source_stream {
	struct bt_bap_stream stream;
	uint16_t seq_num;
	size_t sent_cnt;
	uint8_t *data_ptr;
	uint8_t *start_data_ptr;
	uint8_t *end_data_ptr;
} streams[CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT];
static struct bt_bap_broadcast_source *broadcast_source;

NET_BUF_POOL_FIXED_DEFINE(tx_pool,
			  TOTAL_BUF_NEEDED,
			  BT_ISO_SDU_BUF_SIZE(CONFIG_BT_ISO_TX_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static bool stopping;

static K_SEM_DEFINE(sem_started, 0U, ARRAY_SIZE(streams));
static K_SEM_DEFINE(sem_stopped, 0U, ARRAY_SIZE(streams));

#define BROADCAST_SOURCE_LIFETIME  120U /* seconds */

#define LC3_MIN_FRAME_BYTES        20
#define LC3_MAX_FRAME_BYTES       400

#define CHANNEL_COUNT 1

uint8_t read_buffer[LC3_MAX_FRAME_BYTES * CHANNEL_COUNT];

/**
 * The following section contains the
 * LC3 binary format handler
 *
 * Rewritten handlers to work on uint8_t buffer from
 *
 * https://github.com/google/liblc3/tools/lc3bin.c
 *
 * Original license:
 */
/******************************************************************************
 *
 *  Copyright 2022 Google LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

struct lc3bin_header {
	uint16_t file_id;
	uint16_t header_size;
	uint16_t srate_100hz;
	uint16_t bitrate_100bps;
	uint16_t channels;
	uint16_t frame_10us;
	uint16_t rfu;
	uint16_t nsamples_low;
	uint16_t nsamples_high;
};

int lc3bin_read_header(uint8_t **data, int *frame_us, int *srate_hz,
		       int *nchannels, int *nsamples)
{
	struct lc3bin_header hdr;

	memcpy(&hdr, *data, sizeof(hdr));

	*nchannels = hdr.channels;
	*frame_us = hdr.frame_10us * 10;
	*srate_hz = hdr.srate_100hz * 100;
	*nsamples = hdr.nsamples_low | (hdr.nsamples_high << 16);

	*data += sizeof(hdr);

	return sizeof(hdr);
}

int lc3bin_read_data(struct broadcast_source_stream *source_stream, uint8_t **data, int nchannels, void *buffer)
{
	uint16_t nbytes;

	memcpy(&nbytes, *data, sizeof(nbytes));

	*data += sizeof(nbytes);

	memcpy(buffer, *data, nbytes);

	*data += nbytes;

	if (*data > source_stream->end_data_ptr) {
		*data = source_stream->start_data_ptr;
		printk("End of LC3 array reached => looping., source stream> %p\n", source_stream);
	}

	return nbytes;
}
/****** end of lc3 handler code ******/

static void send_data(struct broadcast_source_stream *source_stream)
{
	struct bt_bap_stream *stream = &source_stream->stream;
	struct net_buf *buf;
	int ret;

	if (stopping) {
		return;
	}

	buf = net_buf_alloc(&tx_pool, K_FOREVER);
	if (buf == NULL) {
		printk("Could not allocate buffer when sending on %p\n",
		       stream);
		return;
	}

	/* read one frame */
	ret = lc3bin_read_data(source_stream, &(source_stream->data_ptr), 1, read_buffer);

	if (ret < 0) {
		printk("ERROR READING LC3 DATA!\n");
		return;
	}

	net_buf_reserve(buf, BT_ISO_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, read_buffer, preset_active.qos.sdu); // TBD

	ret = bt_bap_stream_send(stream, buf, source_stream->seq_num++);
	if (ret < 0) {
		/* This will end broadcasting on this stream. */
		printk("Unable to broadcast data on %p: %d\n", stream, ret);
		net_buf_unref(buf);
		return;
	}

	source_stream->sent_cnt++;
	if ((source_stream->sent_cnt % 1000U) == 0U) {
		printk("Stream %p: Sent %u total ISO packets\n", stream, source_stream->sent_cnt);
	}
}

static void stream_started_cb(struct bt_bap_stream *stream)
{
	struct broadcast_source_stream *source_stream =
		CONTAINER_OF(stream, struct broadcast_source_stream, stream);

	source_stream->seq_num = 0U;
	source_stream->sent_cnt = 0U;
	k_sem_give(&sem_started);
}

static void stream_stopped_cb(struct bt_bap_stream *stream, uint8_t reason)
{
	k_sem_give(&sem_stopped);
}

static void stream_sent_cb(struct bt_bap_stream *stream)
{
	struct broadcast_source_stream *source_stream =
		CONTAINER_OF(stream, struct broadcast_source_stream, stream);

	send_data(source_stream);
}

static struct bt_bap_stream_ops stream_ops = {
	.started = stream_started_cb,
	.stopped = stream_stopped_cb,
	.sent = stream_sent_cb
};

static int setup_broadcast_source(struct bt_bap_broadcast_source **source)
{
	int frame_us;
	int srate_hz;
	int nchannels;
	int nsamples;
	int ret;

	struct bt_bap_broadcast_source_stream_param
		stream_params[CONFIG_BT_BAP_BROADCAST_SRC_STREAM_COUNT];
	struct bt_bap_broadcast_source_subgroup_param
		subgroup_param[CONFIG_BT_BAP_BROADCAST_SRC_SUBGROUP_COUNT];
	struct bt_bap_broadcast_source_param create_param = {0};
	uint8_t left[] = {BT_AUDIO_CODEC_DATA(BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
					      BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_LEFT))};
	uint8_t right[] = {BT_AUDIO_CODEC_DATA(BT_AUDIO_CODEC_CFG_CHAN_ALLOC,
					       BT_BYTES_LIST_LE32(BT_AUDIO_LOCATION_FRONT_RIGHT))};
	int err;

	subgroup_param[0].params_count = 2;
	subgroup_param[0].params = stream_params;
	subgroup_param[0].codec_cfg = &preset_active.codec_cfg;

	for (size_t j = 0U; j < ARRAY_SIZE(stream_params); j++) {
		streams[j].data_ptr = j == 0 ? (uint8_t *)lc3_left : (uint8_t *)lc3_right;

		ret = lc3bin_read_header(&streams[j].data_ptr, &frame_us, &srate_hz, &nchannels, &nsamples);

		printk("LC3 Music header read:\n");
		printk("======================\n");
		printk("Frame size: %dus\n", frame_us);
		printk("Sample rate: %dHz\n", srate_hz);
		printk("Number of channels: %d\n", nchannels);
		printk("Number of samples: %d\n", nsamples);

		/* Store position of first block */
		streams[j].start_data_ptr = streams[j].data_ptr;
		streams[j].end_data_ptr = streams[j].data_ptr + (nsamples / SAMPLES_PER_FRAME) *
			(preset_active.qos.sdu + 2); // TBD

		stream_params[j].stream = &streams[j].stream;
		stream_params[j].data = j == 0 ? left : right;
		stream_params[j].data_len = j == 0 ? sizeof(left) : sizeof(right);
		bt_bap_stream_cb_register(stream_params[j].stream, &stream_ops);
	}

	create_param.params_count = 1;
	create_param.params = subgroup_param;
	create_param.qos = &preset_active.qos;
	create_param.encryption = strlen(CONFIG_BROADCAST_CODE) > 0;
	create_param.packing = BT_ISO_PACKING_SEQUENTIAL;

	err = bt_bap_broadcast_source_create(&create_param, source);
	if (err != 0) {
		printk("Unable to create broadcast source: %d\n", err);
		return err;
	}

	return 0;
}

int main(void)
{
	struct bt_le_ext_adv *adv;
	int err;

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}
	printk("Bluetooth initialized\n");

	/* Broadcast Audio Streaming Endpoint advertising data */
	NET_BUF_SIMPLE_DEFINE(ad_buf,
				BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE);
	NET_BUF_SIMPLE_DEFINE(base_buf, 128);
	struct bt_data ext_ad[3];
	struct bt_data per_ad;
	uint32_t broadcast_id;

	/* Create a non-connectable non-scannable advertising set */
	err = bt_le_ext_adv_create(BT_LE_EXT_ADV_CUSTOM, NULL, &adv);
	if (err != 0) {
		printk("Unable to create extended advertising set: %d\n",
			err);
		return 0;
	}

	/* Set periodic advertising parameters */
	err = bt_le_per_adv_set_param(adv, BT_LE_PER_ADV_DEFAULT);
	if (err) {
		printk("Failed to set periodic advertising parameters"
		" (err %d)\n", err);
		return 0;
	}

	printk("Creating broadcast source\n");
	err = setup_broadcast_source(&broadcast_source);
	if (err != 0) {
		printk("Unable to setup broadcast source: %d\n", err);
		return 0;
	}

	err = bt_bap_broadcast_source_get_id(broadcast_source, &broadcast_id);
	if (err != 0) {
		printk("Unable to get broadcast ID: %d\n", err);
		return 0;
	}

	/* Setup extended advertising data */
	net_buf_simple_add_le16(&ad_buf, BT_UUID_BROADCAST_AUDIO_VAL);
	net_buf_simple_add_le24(&ad_buf, broadcast_id);
	ext_ad[0] = (struct bt_data)BT_DATA(BT_DATA_BROADCAST_NAME, BT_AUDIO_BROADCAST_NAME,
					    sizeof(BT_AUDIO_BROADCAST_NAME) - 1);
	ext_ad[1].type = BT_DATA_SVC_DATA16;
	ext_ad[1].data_len = ad_buf.len;
	ext_ad[1].data = ad_buf.data;
	ext_ad[2] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
					    sizeof(CONFIG_BT_DEVICE_NAME) - 1);
	err = bt_le_ext_adv_set_data(adv, ext_ad, ARRAY_SIZE(ext_ad), NULL, 0);
	if (err != 0) {
		printk("Failed to set extended advertising data: %d\n",
			err);
		return 0;
	}

	/* Setup periodic advertising data */
	err = bt_bap_broadcast_source_get_base(broadcast_source, &base_buf);
	if (err != 0) {
		printk("Failed to get encoded BASE: %d\n", err);
		return 0;
	}

	per_ad.type = BT_DATA_SVC_DATA16;
	per_ad.data_len = base_buf.len;
	per_ad.data = base_buf.data;
	err = bt_le_per_adv_set_data(adv, &per_ad, 1);
	if (err != 0) {
		printk("Failed to set periodic advertising data: %d\n",
			err);
		return 0;
	}

	/* Start extended advertising */
	err = bt_le_ext_adv_start(adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (err) {
		printk("Failed to start extended advertising: %d\n",
			err);
		return 0;
	}

	/* Enable Periodic Advertising */
	err = bt_le_per_adv_start(adv);
	if (err) {
		printk("Failed to enable periodic advertising: %d\n",
			err);
		return 0;
	}

	printk("Starting broadcast source\n");
	err = bt_bap_broadcast_source_start(broadcast_source, adv);
	if (err != 0) {
		printk("Unable to start broadcast source: %d\n", err);
		return 0;
	}

	/* Wait for all to be started */
	for (size_t i = 0U; i < ARRAY_SIZE(streams); i++) {
		k_sem_take(&sem_started, K_FOREVER);
	}
	printk("Broadcast source started\n");

	/* Initialize sending */
	for (size_t i = 0U; i < ARRAY_SIZE(streams); i++) {
		for (unsigned int j = 0U; j < BROADCAST_ENQUEUE_COUNT; j++) {
			stream_sent_cb(&streams[i].stream);
		}
	}

	return 0;
}
