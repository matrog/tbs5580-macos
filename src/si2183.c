/*
 * Silicon Labs Si2183(2) DVB-T/T2/C/C2/S/S2 demodulator driver,
 * user-space port of linux/drivers/media/dvb-frontends/si2183.c
 * (Luis Alves <ljalvs@gmail.com>). Satellite path only.
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 */
#include <stdlib.h>
#include <string.h>
#include "si2183.h"

#define SI2183_PROP_MODE	0x100a
#define SI2183_PROP_DVBS2_SR	0x1401
#define SI2183_PROP_DVBS_SR	0x1501

#define SI2183_ARGLEN      30
struct si2183_cmd {
	u8 args[SI2183_ARGLEN];
	unsigned wlen;
	unsigned rlen;
};

static int si2183_i2c_master_send_unlocked(struct si2183 *dev, const u8 *buf, int count)
{
	struct i2c_msg msg = {
		.addr = dev->cfg.i2c_addr,
		.flags = 0,
		.len = count,
		.buf = (u8 *)buf,
	};
	int ret = i2c_transfer(dev->i2c, &msg, 1);

	return (ret == 1) ? count : ret;
}

static int si2183_i2c_master_recv_unlocked(struct si2183 *dev, u8 *buf, int count)
{
	struct i2c_msg msg = {
		.addr = dev->cfg.i2c_addr,
		.flags = I2C_M_RD,
		.len = count,
		.buf = buf,
	};
	int ret = i2c_transfer(dev->i2c, &msg, 1);

	return (ret == 1) ? count : ret;
}

/* execute firmware command */
static int si2183_cmd_execute_unlocked(struct si2183 *dev, struct si2183_cmd *cmd)
{
	int ret;
	uint64_t start, timeout;

	if (cmd->wlen) {
		/* write cmd and args for firmware */
		ret = si2183_i2c_master_send_unlocked(dev, cmd->args, cmd->wlen);
		if (ret < 0) {
			goto err;
		} else if (ret != (int)cmd->wlen) {
			ret = -EIO;
			goto err;
		}
	}

	if (cmd->rlen) {
		/* wait cmd execution terminate */
		#define TIMEOUT 500
		start = now_ms();
		timeout = start + TIMEOUT;
		while (now_ms() <= timeout) {
			ret = si2183_i2c_master_recv_unlocked(dev, cmd->args, cmd->rlen);
			if (ret < 0) {
				goto err;
			} else if (ret != (int)cmd->rlen) {
				ret = -EIO;
				goto err;
			}

			/* firmware ready? */
			if ((cmd->args[0] >> 7) & 0x01)
				break;
		}

		DBG(2, "si2183: cmd execution took %d ms\n", (int)(now_ms() - start));

		/* error bit set? */
		if ((cmd->args[0] >> 6) & 0x01) {
			ret = -EIO;
			goto err;
		}

		if (!((cmd->args[0] >> 7) & 0x01)) {
			ret = -ETIMEDOUT;
			goto err;
		}
	}

	return 0;
err:
	DBG(1, "si2183: cmd 0x%02x failed=%d\n", cmd->args[0], ret);
	return ret;
}

static int si2183_cmd_execute(struct si2183 *dev, struct si2183_cmd *cmd)
{
	int ret;

	pthread_mutex_lock(&dev->i2c_mutex);
	ret = si2183_cmd_execute_unlocked(dev, cmd);
	pthread_mutex_unlock(&dev->i2c_mutex);
	return ret;
}

static int si2183_set_prop(struct si2183 *dev, u16 prop, u16 *val)
{
	struct si2183_cmd cmd;
	int ret;

	cmd.args[0] = 0x14;
	cmd.args[1] = 0x00;
	cmd.args[2] = (u8) prop;
	cmd.args[3] = (u8) (prop >> 8);
	cmd.args[4] = (u8) (*val);
	cmd.args[5] = (u8) (*val >> 8);
	cmd.wlen = 6;
	cmd.rlen = 4;
	ret = si2183_cmd_execute(dev, &cmd);
	*val = (cmd.args[2] | (cmd.args[3] << 8));
	return ret;
}

static const char *si2183_modulation(u8 v)
{
	switch (v & 0x3f) {
	case 0x03: return "QPSK";
	case 0x07: return "16QAM";
	case 0x08: return "32QAM";
	case 0x09: return "64QAM";
	case 0x0a: return "128QAM";
	case 0x0b: return "256QAM";
	case 0x0e: return "8PSK";
	case 0x14: return "16APSK";
	case 0x17: return "8APSK-L";
	case 0x18: return "16APSK-L";
	case 0x15: return "32APSK";
	case 0x19: return "32APSK-L";
	case 0x1a: return "32APSK";
	default:   return "QPSK";
	}
}

static const char *si2183_fec_dvbs(u8 v)
{
	switch (v & 0x0f) {
	case 0x01: return "1/2";
	case 0x02: return "2/3";
	case 0x03: return "3/4";
	case 0x04: return "4/5";
	case 0x05: return "5/6";
	case 0x06: return "6/7";
	case 0x07: return "7/8";
	default:   return "auto";
	}
}

static const char *si2183_fec_dvbs2(u8 v)
{
	switch (v & 0x1f) {
	case 0x01: return "1/2";
	case 0x02: return "2/3";
	case 0x03: return "3/4";
	case 0x04: return "4/5";
	case 0x05: return "5/6";
	case 0x08: return "8/9";
	case 0x09: return "9/10";
	case 0x0a: return "1/3";
	case 0x0b: return "1/4";
	case 0x0c: return "2/5";
	case 0x0d: return "3/5";
	default:   return "auto";
	}
}

static const char *si2183_rolloff(u8 v)
{
	switch (v & 0x07) {
	case 0x00: return "0.35";
	case 0x01: return "0.25";
	case 0x02: return "0.20";
	case 0x04: return "0.15";
	case 0x05: return "0.10";
	case 0x06: return "0.05";
	default:   return "auto";
	}
}

int si2183_read_status(struct si2183 *dev, const struct fe_params *c, struct fe_status *st)
{
	struct si2183_cmd cmd;
	int ret;

	memset(st, 0, sizeof(*st));

	if (!dev->active)
		return -EAGAIN;

	if (dev->delivery_system != c->delsys || dev->delivery_system == DELSYS_NONE)
		return 0;

	switch (c->delsys) {
	case DELSYS_DVBS:
	case DELSYS_DSS:
		memcpy(cmd.args, "\x60\x01", 2);
		cmd.wlen = 2;
		cmd.rlen = 10;
		break;
	case DELSYS_DVBS2:
		memcpy(cmd.args, "\x70\x01", 2);
		cmd.wlen = 2;
		cmd.rlen = 13;
		break;
	default:
		return -EINVAL;
	}

	ret = si2183_cmd_execute(dev, &cmd);
	if (ret) {
		LOG("si2183: read_status cmd_exec failed=%d\n", ret);
		return ret;
	}

	if (g_verbose >= 2)
		hexdump("si2183: status", cmd.args, cmd.rlen);

	dev->stat_resp = cmd.args[2];
	switch ((dev->stat_resp >> 1) & 0x03) {
	case 0x01:
		st->signal = st->carrier = true;
		break;
	case 0x03:
		st->signal = st->carrier = st->lock = true;
		st->cnr_valid = true;
		st->cnr_mdb = (s32) cmd.args[3] * 250;
		st->modulation = si2183_modulation(cmd.args[8]);
		if (c->delsys == DELSYS_DVBS2) {
			st->fec = si2183_fec_dvbs2(cmd.args[9]);
			st->rolloff = si2183_rolloff(cmd.args[10]);
			st->pilot = ((cmd.args[8] >> 7) & 0x01) ? "on" : "off";
		} else {
			st->fec = si2183_fec_dvbs(cmd.args[9]);
		}
		break;
	default:
		break;
	}
	return 0;
}

int si2183_read_sat_agc(struct si2183 *dev, u16 *agc)
{
	struct si2183_cmd cmd;
	int ret;

	memcpy(cmd.args, "\x8a\x00\x00\x00\x00\x00", 6);
	cmd.wlen = 6;
	cmd.rlen = 3;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		return ret;
	*agc = cmd.args[1];
	return 0;
}

int si2183_read_ber(struct si2183 *dev, u32 *ber)
{
	struct si2183_cmd cmd;
	int ret;

	memcpy(cmd.args, "\x82\x00", 2);
	cmd.wlen = 2;
	cmd.rlen = 3;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		return ret;
	*ber = (u32)cmd.args[2] * cmd.args[1] & 0xf;
	return 0;
}

int si2183_read_ucblocks(struct si2183 *dev, u32 *ucblocks)
{
	struct si2183_cmd cmd;
	int ret;

	*ucblocks = 0;
	if (!(dev->stat_resp & 0x10))
		return 0;

	memcpy(cmd.args, "\x84\x00", 2);
	cmd.wlen = 2;
	cmd.rlen = 3;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		return ret;
	*ucblocks = (u16)cmd.args[2] << 8 | cmd.args[1];
	return 0;
}

static int gold_code_index(int gold_sequence_index)
{
	unsigned int i, k, x_init;
	u8 GOLD_PRBS[19] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	for (k = 0; k < (unsigned)gold_sequence_index; k++) {
		GOLD_PRBS[18] = (GOLD_PRBS[0] + GOLD_PRBS[7]) % 2;
		/* Shifting 18 first values */
		for (i = 0; i < 18; i++)
			GOLD_PRBS[i] = GOLD_PRBS[i + 1];
	}
	x_init = 0;
	for (i = 0; i < 18; i++)
		x_init = x_init + GOLD_PRBS[i] * (1 << i);

	return x_init;
}

static int si2183_set_dvbs(struct si2183 *dev, const struct fe_params *c)
{
	struct si2183_cmd cmd;
	int ret;
	u16 prop;
	u32 pls_mode, pls_code;

	/* set SAT agc */
	memcpy(cmd.args, "\x8a\x1d\x12\x0\x0\x0", 6);
	cmd.args[1] = dev->cfg.agc_mode | 0x18;
	cmd.wlen = 6;
	cmd.rlen = 3;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		LOG("si2183: err set agc mode\n");

	/* set mode */
	prop = 0x8;
	switch (c->delsys) {
	default:
	case DELSYS_DVBS:
		prop |= 0x80;
		break;
	case DELSYS_DVBS2:
		prop |= 0x90;
		break;
	case DELSYS_DSS:
		prop |= 0xa0;
		break;
	}
	if (c->inversion)
		prop |= 0x100;
	ret = si2183_set_prop(dev, SI2183_PROP_MODE, &prop);
	if (ret) {
		LOG("si2183: err set dvb-s/s2 mode\n");
		return ret;
	}

	/* symbol rate */
	prop = c->symbol_rate / 1000;
	switch (c->delsys) {
	default:
	case DELSYS_DSS:
	case DELSYS_DVBS:
		ret = si2183_set_prop(dev, SI2183_PROP_DVBS_SR, &prop);
		break;
	case DELSYS_DVBS2:
		ret = si2183_set_prop(dev, SI2183_PROP_DVBS2_SR, &prop);
		/* stream_id selection */
		cmd.args[0] = 0x71;
		cmd.args[1] = (u8) c->stream_id;
		cmd.args[2] = c->stream_id == NO_STREAM_ID_FILTER ? 0 : 1;
		cmd.wlen = 3;
		cmd.rlen = 1;
		ret = si2183_cmd_execute(dev, &cmd);
		if (ret)
			LOG("si2183: dvb-s2: err selecting stream_id\n");

		/* pls selection */
		pls_mode = c->stream_id == NO_STREAM_ID_FILTER ? 0 : (c->stream_id >> 26) & 3;
		pls_code = c->stream_id == NO_STREAM_ID_FILTER ? 0 : (c->stream_id >> 8) & 0x3FFFF;
		if (pls_mode)
			pls_code = gold_code_index(pls_code);
		cmd.args[0] = 0x73;
		cmd.args[1] = pls_code > 0;
		cmd.args[2] = cmd.args[3] = 0;
		cmd.args[4] = (u8) pls_code;
		cmd.args[5] = (u8) (pls_code >> 8);
		cmd.args[6] = (u8) (pls_code >> 16);
		cmd.args[7] = (u8) (pls_code >> 24);
		cmd.wlen = 8;
		cmd.rlen = 1;
		ret = si2183_cmd_execute(dev, &cmd);
		if (ret)
			LOG("si2183: dvb-s2: err set pls\n");
	}

	return 0;
}

int si2183_set_frontend(struct si2183 *dev, const struct fe_params *c)
{
	struct si2183_cmd cmd;
	int ret;

	DBG(1, "si2183: delivery_system=%u frequency=%u symbol_rate=%u inversion=%u stream_id=%d\n",
	    c->delsys, c->frequency, c->symbol_rate, c->inversion, (int)c->stream_id);

	if (!dev->active)
		return -EAGAIN;

	switch (c->delsys) {
	case DELSYS_DVBS:
	case DELSYS_DVBS2:
	case DELSYS_DSS:
		ret = si2183_set_dvbs(dev, c);
		break;
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;

	/* dsp restart */
	memcpy(cmd.args, "\x85", 1);
	cmd.wlen = 1;
	cmd.rlen = 1;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret) {
		LOG("si2183: err restarting dsp\n");
		return ret;
	}

	dev->delivery_system = c->delsys;
	return 0;
}

static int read_file(const char *path, u8 **data, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return -ENOENT;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = malloc(n > 0 ? n : 1);
	if (!*data) {
		fclose(f);
		return -ENOMEM;
	}
	*size = fread(*data, 1, n, f);
	fclose(f);
	return (long)*size == n ? 0 : -EIO;
}

int si2183_init(struct si2183 *dev)
{
	int ret = 0, len, remaining;
	u8 *fw;
	size_t fw_size;
	struct si2183_cmd cmd;
	unsigned int chip_id;
	u16 prop;

	if (dev->active)
		return 0;

	/* initialize */
	memcpy(cmd.args, "\xc0\x12\x00\x0c\x00\x0d\x16\x00\x00\x00\x00\x00\x00", 13);
	if (dev->cfg.start_clk_mode == 1) {
		cmd.args[3] = 0;
		cmd.args[5] = 0x6;
	}
	cmd.wlen = 13;
	cmd.rlen = 0;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		goto err;

	if (dev->fw_loaded) {
		/* resume */
		memcpy(cmd.args, "\xc0\x06\x08\x0f\x00\x20\x21\x01", 8);
		if (dev->cfg.start_clk_mode == 1)
			cmd.args[6] = 0x31;
		cmd.wlen = 8;
		cmd.rlen = 1;
		ret = si2183_cmd_execute(dev, &cmd);
		if (ret)
			goto err;

		memcpy(cmd.args, "\x85", 1);
		cmd.wlen = 1;
		cmd.rlen = 1;
		ret = si2183_cmd_execute(dev, &cmd);
		if (ret)
			goto err;

		goto warm;
	}

	/* power up */
	memcpy(cmd.args, "\xc0\x06\x01\x0f\x00\x20\x20\x01", 8);
	if (dev->cfg.start_clk_mode == 1)
		cmd.args[6] = 0x30;
	cmd.wlen = 8;
	cmd.rlen = 1;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		goto err;

	/* query chip revision */
	memcpy(cmd.args, "\x02", 1);
	cmd.wlen = 1;
	cmd.rlen = 13;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		goto err;

	chip_id = cmd.args[1] << 24 | cmd.args[2] << 16 | cmd.args[3] << 8 |
		  cmd.args[4] << 0;

	#define SI2183_B60 ('B' << 24 | 83 << 16 | '6' << 8 | '0' << 0)

	if (chip_id != SI2183_B60) {
		LOG("si2183: unknown chip version Si21%d-%c%c%c\n",
		    cmd.args[2], cmd.args[1], cmd.args[3], cmd.args[4]);
		ret = -EINVAL;
		goto err;
	}

	LOG("found a 'Silicon Labs Si21%d-%c%c%c'\n",
	    cmd.args[2], cmd.args[1], cmd.args[3], cmd.args[4]);

	ret = read_file(dev->cfg.fw_path, &fw, &fw_size);
	if (ret) {
		LOG("si2183: firmware file '%s' not found\n", dev->cfg.fw_path);
		goto err;
	}

	LOG("downloading demod firmware from file '%s'\n", dev->cfg.fw_path);

	for (remaining = fw_size; remaining > 0; remaining -= 17) {
		len = fw[fw_size - remaining];
		if (len > SI2183_ARGLEN) {
			ret = -EINVAL;
			break;
		}
		memcpy(cmd.args, &fw[(fw_size - remaining) + 1], len);
		cmd.wlen = len;
		cmd.rlen = 1;
		ret = si2183_cmd_execute(dev, &cmd);
		if (ret)
			break;
	}
	free(fw);

	if (ret) {
		LOG("si2183: firmware download failed %d\n", ret);
		goto err;
	}

	memcpy(cmd.args, "\x01\x01", 2);
	cmd.wlen = 2;
	cmd.rlen = 1;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		goto err;

	/* query firmware version */
	memcpy(cmd.args, "\x11", 1);
	cmd.wlen = 1;
	cmd.rlen = 10;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		goto err;

	LOG("demod firmware version: %c.%c.%d\n", cmd.args[6], cmd.args[7], cmd.args[8]);

	/* set ts mode */
	prop = 0x10 | dev->cfg.ts_mode | (dev->cfg.ts_clock_gapped ? 0x40 : 0);
	ret = si2183_set_prop(dev, 0x1001, &prop);
	if (ret)
		LOG("si2183: err set ts mode\n");

	/* FER resol */
	prop = 0x12;
	ret = si2183_set_prop(dev, 0x100c, &prop);
	if (ret) {
		LOG("si2183: err set FER resol\n");
		return ret;
	}

	/* DD IEN */
	prop = 0x00;
	ret = si2183_set_prop(dev, 0x1006, &prop);
	if (ret) {
		LOG("si2183: err set dd ien\n");
		return ret;
	}

	/* int sense */
	prop = 0x2000;
	ret = si2183_set_prop(dev, 0x1007, &prop);
	if (ret) {
		LOG("si2183: err set int sense\n");
		return ret;
	}

	/* Control of SQI computation */
	prop = 0x1e;
	ret = si2183_set_prop(dev, 0x100f, &prop);
	if (ret) {
		LOG("si2183: err set sqi comp\n");
		return ret;
	}

	/* Transport Stream setting for parallel mode */
	prop = 0x0104 | (dev->cfg.ts_clock_inv ? 0x0000 : 0x1000);
	ret = si2183_set_prop(dev, 0x1009, &prop);
	if (ret) {
		LOG("si2183: err set par_ts\n");
		return ret;
	}

	/* Transport Stream setting for serial mode */
	prop = 0x230C | (dev->cfg.ts_clock_inv ? 0x0000 : 0x1000);
	ret = si2183_set_prop(dev, 0x1008, &prop);
	if (ret) {
		LOG("si2183: err set ser_ts\n");
		return ret;
	}

	/* Transport Stream setting for parallel mode - secondary*/
	prop = 0x08e3;
	ret = si2183_set_prop(dev, 0x1015, &prop);
	if (ret) {
		LOG("si2183: err set int par_ts_sec\n");
		return ret;
	}

	/* Transport Stream setting for serial mode - secondary*/
	prop = 0x01c7;
	ret = si2183_set_prop(dev, 0x1016, &prop);
	if (ret) {
		LOG("si2183: err set int ser_ts_sec\n");
		return ret;
	}

	dev->fw_loaded = true;
warm:
	dev->active = true;
	return 0;

err:
	LOG("si2183: init failed=%d\n", ret);
	return ret;
}

int si2183_sleep(struct si2183 *dev)
{
	struct si2183_cmd cmd;

	dev->active = false;

	memcpy(cmd.args, "\x13", 1);
	cmd.wlen = 1;
	cmd.rlen = 0;
	return si2183_cmd_execute(dev, &cmd);
}

static int send_diseqc_cmd(struct si2183 *dev,
	u8 cont_tone, u8 tone_burst, u8 burst_sel,
	u8 end_seq, u8 msg_len, const u8 *msg)
{
	struct si2183_cmd cmd;
	u8 enable = 1;

	memset(cmd.args, 0, sizeof(cmd.args));
	cmd.args[0] = 0x8c;
	cmd.args[1] = enable | (cont_tone << 1)
		    | (tone_burst << 2) | (burst_sel << 3)
		    | (end_seq << 4) | (msg_len << 5);

	if (msg_len > 0)
		memcpy(&cmd.args[2], msg, msg_len);

	cmd.wlen = 8;
	cmd.rlen = 1;
	return si2183_cmd_execute(dev, &cmd);
}

int si2183_set_tone(struct si2183 *dev, bool on)
{
	int ret = send_diseqc_cmd(dev, on ? 1 : 0, 0, 0, 1, 0, NULL);

	if (ret)
		LOG("si2183: set_tone failed=%d\n", ret);
	return ret;
}

int si2183_diseqc_send_burst(struct si2183 *dev, bool mini_b)
{
	int ret = send_diseqc_cmd(dev, 0, 1, mini_b ? 1 : 0, 1, 0, NULL);

	if (ret)
		LOG("si2183: send_burst failed=%d\n", ret);
	return ret;
}

int si2183_diseqc_send_msg(struct si2183 *dev, const u8 *msg, int msg_len)
{
	int ret;
	int remaining = msg_len;
	const u8 *p = msg;
	int len = 0;

	while (remaining > 0) {
		p += len;
		len = (remaining > 6) ? 6 : remaining;
		remaining -= len;
		ret = send_diseqc_cmd(dev, 0, 0, 0, (remaining == 0) ? 1 : 0, len, p);
		if (ret) {
			LOG("si2183: diseqc_send_msg failed=%d\n", ret);
			return ret;
		}
		msleep(50);
	}
	return 0;
}

/* ---- hardware blind scan ----------------------------------------------- */
/*
 * Based on si2183_blindscan.c from deeptho/linux_media (neumoDVB),
 * (c) 2020-2025 Deep Thought <deeptho@gmail.com>, GPL v2 or later.
 */

#define SI2183_SCAN_STATUS_CMD		0x30
#define SI2183_SCAN_CTRL_CMD		0x31
#define SI2183_SCAN_CTRL_START		1
#define SI2183_SCAN_CTRL_RESUME		2
#define SI2183_SCAN_CTRL_ABORT		3

#define SI2183_SCAN_STATUS_SEARCHING	1
#define SI2183_SCAN_STATUS_ENDED	2
#define SI2183_SCAN_STATUS_ERROR	3
#define SI2183_SCAN_STATUS_TUNE_REQUEST	4
#define SI2183_SCAN_STATUS_DIGITAL_FOUND 5
#define SI2183_SCAN_STATUS_DEBUG	63

#define SI2183_PROP_SCAN_SAT_CONFIG	0x0302
#define SI2183_PROP_SCAN_FMIN		0x0303
#define SI2183_PROP_SCAN_FMAX		0x0304
#define SI2183_PROP_SCAN_SR_MIN		0x0305
#define SI2183_PROP_SCAN_SR_MAX		0x0306
#define SI2183_PROP_SCAN_INT_SENSE	0x0307
#define SI2183_PROP_SCAN_IEN		0x0308

struct si2183_scan_status {
	bool buz;
	u8 status;
	u32 rf_freq;
	u16 symb_rate;
	u8 modulation;
};

static int si2183_scan_status(struct si2183 *dev, bool intack, struct si2183_scan_status *s)
{
	struct si2183_cmd cmd;
	int ret;

	cmd.args[0] = SI2183_SCAN_STATUS_CMD;
	cmd.args[1] = intack ? 1 : 0;
	cmd.wlen = 2;
	cmd.rlen = 11;
	ret = si2183_cmd_execute(dev, &cmd);
	if (ret)
		return ret;
	s->buz = cmd.args[2] & 0x01;
	s->status = cmd.args[3] & 0x3f;
	s->rf_freq = cmd.args[4] | cmd.args[5] << 8 | cmd.args[6] << 16 | (u32)cmd.args[7] << 24;
	s->symb_rate = cmd.args[8] | cmd.args[9] << 8;
	s->modulation = cmd.args[10] & 0x0f;
	DBG(2, "si2183: scan status=%d buz=%d freq=%u sr=%u mod=%u\n", s->status, s->buz,
	    s->rf_freq, s->symb_rate, s->modulation);
	return 0;
}

static int si2183_scan_ctrl(struct si2183 *dev, u8 action, u32 seek)
{
	struct si2183_cmd cmd;

	cmd.args[0] = SI2183_SCAN_CTRL_CMD;
	cmd.args[1] = action;
	cmd.args[2] = 0;
	cmd.args[3] = 0;
	cmd.args[4] = seek & 0xff;
	cmd.args[5] = (seek >> 8) & 0xff;
	cmd.args[6] = (seek >> 16) & 0xff;
	cmd.args[7] = (seek >> 24) & 0xff;
	cmd.wlen = 8;
	cmd.rlen = 1;
	return si2183_cmd_execute(dev, &cmd);
}

/* Reads the status byte: has the scan engine raised an interrupt? */
static int si2183_scan_int(struct si2183 *dev, bool *scanint)
{
	struct si2183_cmd cmd;
	int ret;

	cmd.wlen = 0;
	cmd.rlen = 1;
	ret = si2183_cmd_execute(dev, &cmd);
	*scanint = !ret && ((cmd.args[0] >> 1) & 0x01);
	return ret;
}

static int si2183_scan_wait_idle(struct si2183 *dev, struct si2183_scan_status *s)
{
	uint64_t deadline = now_ms() + 5000;
	int ret;

	while ((ret = si2183_scan_status(dev, false, s)) == 0 && s->buz) {
		if (now_ms() > deadline)
			return -ETIMEDOUT;
		msleep(20);
	}
	return ret;
}

void si2183_blindscan_abort(struct si2183 *dev)
{
	si2183_scan_ctrl(dev, SI2183_SCAN_CTRL_ABORT, 0);
	dev->bs_started = false;
}

int si2183_blindscan_start(struct si2183 *dev, u32 fmin, u32 fmax, u32 sr_min, u32 sr_max)
{
	struct si2183_cmd cmd;
	u16 prop;
	int ret = 0;

	if (!dev->active)
		return -EAGAIN;

	/* set SAT agc, as for a normal DVB-S tune */
	memcpy(cmd.args, "\x8a\x1d\x12\x0\x0\x0", 6);
	cmd.args[1] = dev->cfg.agc_mode | 0x18;
	cmd.wlen = 6;
	cmd.rlen = 3;
	ret |= si2183_cmd_execute(dev, &cmd);

	si2183_blindscan_abort(dev);

	/* scan limits: frequencies in units of 65.536 kHz, symbol rates in ksym/s */
	prop = (u16)(((uint64_t)fmin * 1000) >> 16);
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_FMIN, &prop);
	prop = (u16)(((uint64_t)fmax * 1000) >> 16);
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_FMAX, &prop);
	prop = sr_min / 1000;
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_SR_MIN, &prop);
	prop = sr_max / 1000;
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_SR_MAX, &prop);

	/* scan interrupts: buzien + reqien, buz on falling edge, req on rising edge */
	prop = 0x03;
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_IEN, &prop);
	prop = (1 << 0) | (1 << 9);
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_INT_SENSE, &prop);

	/* DD_MODE: modulation auto-detect, any DVB-S/S2 */
	prop = 0x8 | (15 << 4) | (2 << 9);
	ret |= si2183_set_prop(dev, SI2183_PROP_MODE, &prop);

	/* SCAN_SAT_CONFIG: analog_detect, reserved2 = 12, no debug */
	prop = 1 | (12 << 6);
	ret |= si2183_set_prop(dev, SI2183_PROP_SCAN_SAT_CONFIG, &prop);

	/* dsp restart */
	memcpy(cmd.args, "\x85", 1);
	cmd.wlen = 1;
	cmd.rlen = 1;
	ret |= si2183_cmd_execute(dev, &cmd);

	dev->delivery_system = DELSYS_NONE;
	dev->bs_seek = fmin;
	dev->bs_started = false;
	return ret ? -EIO : 0;
}

int si2183_blindscan_next(struct si2183 *dev, si2183_tune_cb tune, void *opaque,
			  struct si2183_bs_result *res, volatile int *stop)
{
	struct si2183_scan_status s;
	int ret;

	if (!dev->bs_started)
		tune(opaque, dev->bs_seek);

	ret = si2183_scan_wait_idle(dev, &s);
	if (ret)
		return ret;
	ret = si2183_scan_ctrl(dev, dev->bs_started ? SI2183_SCAN_CTRL_RESUME :
			       SI2183_SCAN_CTRL_START, dev->bs_seek);
	if (ret)
		return ret;
	dev->bs_started = true;

	for (;;) {
		bool scanint, resume = true;

		if (stop && *stop) {
			si2183_blindscan_abort(dev);
			return -EINTR;
		}
		ret = si2183_scan_int(dev, &scanint);
		if (ret)
			return ret;
		if (!scanint) {
			msleep(50);
			continue;
		}

		ret = si2183_scan_status(dev, true, &s);
		if (!ret && s.buz)
			ret = si2183_scan_wait_idle(dev, &s);
		if (ret)
			return ret;

		switch (s.status) {
		case SI2183_SCAN_STATUS_TUNE_REQUEST:
			dev->bs_seek = s.rf_freq;
			tune(opaque, dev->bs_seek);
			msleep(100);
			break;
		case SI2183_SCAN_STATUS_DIGITAL_FOUND:
			res->frequency = s.rf_freq;
			res->symbol_rate = s.symb_rate * 1000;
			res->delsys = s.modulation == 9 ? DELSYS_DVBS2 : DELSYS_DVBS;
			/* clear scanint so the next resume does not see it again */
			si2183_scan_status(dev, true, &s);
			return 1;
		case SI2183_SCAN_STATUS_ENDED:
			dev->bs_started = false;
			return 0;
		case SI2183_SCAN_STATUS_ERROR:
			dev->bs_started = false;
			return -EIO;
		case SI2183_SCAN_STATUS_DEBUG:
			break;
		case SI2183_SCAN_STATUS_SEARCHING:
		default:
			resume = false;
			break;
		}
		if (resume) {
			ret = si2183_scan_ctrl(dev, SI2183_SCAN_CTRL_RESUME, dev->bs_seek);
			if (ret)
				return ret;
		}
	}
}

/*
 * Tuner bus behind the demod: open the I2C gate around every transfer,
 * as si2183_select()/si2183_deselect() do through the Linux i2c-mux.
 */
static int si2183_gate(struct si2183 *dev, bool open)
{
	struct si2183_cmd cmd;

	memcpy(cmd.args, "\xc0\x0d\x00", 3);
	cmd.args[2] = open ? 1 : 0;
	cmd.wlen = 3;
	cmd.rlen = 0;
	return si2183_cmd_execute_unlocked(dev, &cmd);
}

static int si2183_tuner_xfer(void *priv, struct i2c_msg *msgs, int num)
{
	struct si2183 *dev = priv;
	int ret;

	pthread_mutex_lock(&dev->i2c_mutex);
	ret = si2183_gate(dev, true);
	if (ret == 0) {
		ret = i2c_transfer(dev->i2c, msgs, num);
		si2183_gate(dev, false);
	}
	pthread_mutex_unlock(&dev->i2c_mutex);
	return ret;
}

void si2183_attach(struct si2183 *dev, struct i2c_bus *i2c, const struct si2183_config *cfg)
{
	memset(dev, 0, sizeof(*dev));
	dev->cfg = *cfg;
	dev->i2c = i2c;
	pthread_mutex_init(&dev->i2c_mutex, NULL);
	dev->tuner_i2c.xfer = si2183_tuner_xfer;
	dev->tuner_i2c.priv = dev;
}
