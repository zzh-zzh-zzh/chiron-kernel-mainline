/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded reader for the original Chiron TI PPC firmware format. */
struct tas_reader {
	const unsigned char *p;
	size_t left;
	int error;
};

static const unsigned char *tas_take(struct tas_reader *r, size_t n)
{
	const unsigned char *p = r->p;

	if (r->error || n > r->left) {
		r->error = r->error ?: -EINVAL;
		return NULL;
	}
	r->p += n;
	r->left -= n;
	return p;
}

static unsigned int tas_number(struct tas_reader *r, unsigned int bytes)
{
	const unsigned char *p = tas_take(r, bytes);
	unsigned int v = 0, i;

	if (p)
		for (i = 0; i < bytes; i++)
			v = (v << 8) | p[i];
	return v;
}

static void tas_name(struct tas_reader *r, char name[64], char **description)
{
	const unsigned char *p = tas_take(r, 64), *end;
	size_t len;

	if (!p)
		return;
	memcpy(name, p, 64);
	name[63] = 0;
	end = memchr(r->p, 0, r->left);
	if (!end) {
		r->error = -EINVAL;
		return;
	}
	len = end - r->p + 1;
	p = tas_take(r, len);
	*description = kmemdup(p, len, GFP_KERNEL);
	if (!*description)
		r->error = -ENOMEM;
}

/* Reject unknown opcodes, unbounded delays and bursts crossing a page. */
static int tas_commands_valid(const unsigned char *p, size_t len)
{
	size_t i = 0, n, words;
	unsigned int delay = 0;

	while (i < len) {
		if (len - i < 4)
			return -EINVAL;
		n = ((unsigned int)p[i] << 8) | p[i + 1];
		switch (p[i + 2]) {
		case 0 ... 0x7f:
			i += 4;
			break;
		case 0x81:
			delay += n;
			if (delay > 1000)
				return -EINVAL;
			i += 4;
			break;
		case 0x85:
			i += 4;
			words = (n + 6) / 4 * 4;
			if (!n || n > 128 || words > len - i ||
			    p[i + 2] + n > 128)
				return -EINVAL;
			i += words;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static void tas_block(struct tas_reader *r, struct TFirmware *f, struct TBlock *b)
{
	const unsigned char *p;
	size_t bytes;

	b->mnType = tas_number(r, 4);
	if (f->mnDriverVersion >= PPC_DRIVER_CRCCHK) {
		b->mbPChkSumPresent = tas_number(r, 1);
		b->mnPChkSum = tas_number(r, 1);
		b->mbYChkSumPresent = tas_number(r, 1);
		b->mnYChkSum = tas_number(r, 1);
	}
	b->mnCommands = tas_number(r, 4);
	if (b->mnCommands > r->left / 4) {
		r->error = -EINVAL;
		return;
	}
	bytes = (size_t)b->mnCommands * 4;
	p = tas_take(r, bytes);
	if (!p)
		return;
	if (tas_commands_valid(p, bytes)) {
		r->error = -EINVAL;
		return;
	}
	b->mpData = kmemdup(p, bytes, GFP_KERNEL);
	if (!b->mpData)
		r->error = -ENOMEM;
}

static void tas_data(struct tas_reader *r, struct TFirmware *f, struct TData *d)
{
	unsigned int i, n;

	tas_name(r, d->mpName, &d->mpDescription);
	n = tas_number(r, 2);
	if (r->error || n > 64) {
		r->error = r->error ?: -EINVAL;
		return;
	}
	d->mpBlocks = kcalloc(n, sizeof(*d->mpBlocks), GFP_KERNEL);
	if (!d->mpBlocks) {
		r->error = -ENOMEM;
		return;
	}
	d->mnBlocks = n;
	for (i = 0; i < n && !r->error; i++)
		tas_block(r, f, &d->mpBlocks[i]);
}

static int fw_parse(struct tas2559_priv *tas, struct TFirmware *f,
		    unsigned char *data, unsigned int size)
{
	struct tas_reader r = { .p = data, .left = size };
	unsigned int i, n;
	const unsigned char *magic;

	if (size < 104 || size > 256 * 1024)
		return -EINVAL;
	magic = tas_take(&r, 4);
	if (memcmp(magic, "5552", 4))
		return -EINVAL;
	f->mnFWSize = tas_number(&r, 4);
	f->mnChecksum = tas_number(&r, 4);
	f->mnPPCVersion = tas_number(&r, 4);
	f->mnFWVersion = tas_number(&r, 4);
	f->mnDriverVersion = tas_number(&r, 4);
	f->mnTimeStamp = tas_number(&r, 4);
	tas_name(&r, f->mpDDCName, &f->mpDescription);
	f->mnDeviceFamily = tas_number(&r, 4);
	f->mnDevice = tas_number(&r, 4);
	if (r.error || f->mnFWSize != size || f->mnDeviceFamily || f->mnDevice != 4)
		goto invalid;
	/* Only the original Chiron PPC format is supported by this port. */
	if (f->mnDriverVersion != 0x300)
		goto invalid;
	n = tas_number(&r, 2);
	if (!n || n > 32 || r.error)
		goto invalid;
	f->mpPLLs = kcalloc(n, sizeof(*f->mpPLLs), GFP_KERNEL);
	if (!f->mpPLLs)
		goto nomem;
	f->mnPLLs = n;
	for (i = 0; i < n && !r.error; i++) {
		tas_name(&r, f->mpPLLs[i].mpName, &f->mpPLLs[i].mpDescription);
		tas_block(&r, f, &f->mpPLLs[i].mBlock);
	}
	n = tas_number(&r, 2);
	if (!n || n > 16 || r.error)
		goto invalid;
	f->mpPrograms = kcalloc(n, sizeof(*f->mpPrograms), GFP_KERNEL);
	if (!f->mpPrograms)
		goto nomem;
	f->mnPrograms = n;
	for (i = 0; i < n && !r.error; i++) {
		struct TProgram *v = &f->mpPrograms[i];
		tas_name(&r, v->mpName, &v->mpDescription);
		v->mnAppMode = tas_number(&r, 1);
		v->mnBoost = tas_number(&r, 2);
		tas_data(&r, f, &v->mData);
	}
	n = tas_number(&r, 2);
	if (!n || n > 64 || r.error)
		goto invalid;
	f->mpConfigurations = kcalloc(n, sizeof(*f->mpConfigurations), GFP_KERNEL);
	if (!f->mpConfigurations)
		goto nomem;
	f->mnConfigurations = n;
	for (i = 0; i < n && !r.error; i++) {
		struct TConfiguration *v = &f->mpConfigurations[i];
		tas_name(&r, v->mpName, &v->mpDescription);
		v->mnDevices = tas_number(&r, 2);
		v->mnProgram = tas_number(&r, 1);
		v->mnPLL = tas_number(&r, 1);
		v->mnSamplingRate = tas_number(&r, 4);
		if (v->mnDevices < 1 || v->mnDevices > 3 ||
		    v->mnProgram >= f->mnPrograms || v->mnPLL >= f->mnPLLs ||
		    v->mnSamplingRate != 48000)
			goto invalid;
		tas_data(&r, f, &v->mData);
	}
	/* Original files have no external calibration payload. */
	if (r.error || (r.left && (r.left != 2 || tas_number(&r, 2))))
		goto invalid;
	return 0;
nomem:
	r.error = -ENOMEM;
invalid:
	tas2559_clear_firmware(f);
	return r.error ?: -EINVAL;
}
