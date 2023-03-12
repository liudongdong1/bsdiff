#include "bsdiff.h"
#include "bsdiff_private.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

int bsdiff_create_bz2_compressor(struct bsdiff_compressor *enc);
int bsdiff_create_bz2_decompressor(struct bsdiff_decompressor *dec);
/**
 * @brief calculate the size of 8 bytes，one byte equal to 8 bits
 * 
 * @param buf 
 * @return int64_t 
 */
static int64_t offtin(uint8_t *buf)
{
	int64_t y;

	y = buf[7] & 0x7F;
	y = y * 256; y += buf[6];
	y = y * 256; y += buf[5];
	y = y * 256; y += buf[4];
	y = y * 256; y += buf[3];
	y = y * 256; y += buf[2];
	y = y * 256; y += buf[1];
	y = y * 256; y += buf[0];
	// flag position
	if (buf[7] & 0x80)
		y = -y;

	return y;
}

static void offtout(int64_t x, uint8_t *buf)
{
	int64_t y;

	if (x < 0)
		y = -x;
	else
		y = x;

				 buf[0] = y % 256; y -= buf[0];
	y = y / 256; buf[1] = y % 256; y -= buf[1];
	y = y / 256; buf[2] = y % 256; y -= buf[2];
	y = y / 256; buf[3] = y % 256; y -= buf[3];
	y = y / 256; buf[4] = y % 256; y -= buf[4];
	y = y / 256; buf[5] = y % 256; y -= buf[5];
	y = y / 256; buf[6] = y % 256; y -= buf[6];
	y = y / 256; buf[7] = y % 256;

	if (x < 0)
		buf[7] |= 0x80;
}

struct bz2_patch_packer
{
	struct bsdiff_stream *stream;
	int mode;

	int64_t new_size;

	int64_t header_x;
	int64_t header_y;
	int64_t header_z;

	struct bsdiff_stream cpf;
	struct bsdiff_stream dpf;
	struct bsdiff_stream epf;
	struct bsdiff_decompressor cpf_dec;  // control block
	struct bsdiff_decompressor dpf_dec;  // diff block
	struct bsdiff_decompressor epf_dec;  // extra block

	struct bsdiff_compressor enc;   //comress data, 
	uint8_t *db;  //bz2_patch_packer_write_entry_diff save to
	uint8_t *eb;   //bz2_patch_packer_write_entry_extra save to
	int64_t dblen;
	int64_t eblen;
};
/**
 * @brief read bz2_patch_packer, and get the new size
 * 
 * @param state point address of bz2_patch_packer
 * @param size store the sizeof(newfile)
 * @return int 
 */
static int bz2_patch_packer_read_new_size(void *state, int64_t *size)
{
	int ret;
	uint8_t header[32];
	size_t cb;
	int64_t bzctrllen, bzdatalen, newsize;
	int64_t read_start, read_end;

	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_READ);
	if (packer->new_size >= 0) {
		*size = packer->new_size;
		return BSDIFF_SUCCESS;
	}
	assert(packer->new_size == -1);  

	/*
	File format:
		0		8	"BSDIFF40"
		8		8	X
		16		8	Y
		24		8	sizeof(newfile)
		32		X	bzip2(control block)
		32+X	Y	bzip2(diff block)
		32+X+Y	???	bzip2(extra block)
	with control block a set of triples (x,y,z) meaning "add x bytes
	from oldfile to x bytes from the diff block; copy y bytes from the
	extra block; seek forwards in oldfile by z bytes".
	*/

	/* Read header */
	ret = packer->stream->read(packer->stream->state, header, 32, &cb);
	if (ret != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;

	/* Check for appropriate magic */
	if (memcmp(header, "BSDIFF40", 8) != 0)
		return BSDIFF_CORRUPT_PATCH;

	/* Read lengths from header */
	bzctrllen = offtin(header + 8);
	bzdatalen = offtin(header + 16);
	newsize = offtin(header + 24);
	if ((bzctrllen < 0) || (bzdatalen < 0) || (newsize < 0))
		return BSDIFF_CORRUPT_PATCH;

	/* Open substreams and create decompressors */
	/* control block */
	read_start = 32;
	read_end = read_start + bzctrllen;
	if (bsdiff_open_substream(packer->stream, read_start, read_end, &(packer->cpf)) != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;
	if (bsdiff_create_bz2_decompressor(&(packer->cpf_dec)) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	if (packer->cpf_dec.init(packer->cpf_dec.state, &(packer->cpf)) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	/* diff block */
	read_start = read_end;
	read_end = read_start + bzdatalen;
	if (bsdiff_open_substream(packer->stream, read_start, read_end, &(packer->dpf)) != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;
	if (bsdiff_create_bz2_decompressor(&(packer->dpf_dec)) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	if (packer->dpf_dec.init(packer->dpf_dec.state, &(packer->dpf)) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	/* extra block */
	read_start = read_end;
	if ((packer->stream->seek(packer->stream->state, 0, BSDIFF_SEEK_END) != BSDIFF_SUCCESS) ||
		(packer->stream->tell(packer->stream->state, &read_end) != BSDIFF_SUCCESS))
	{
		return BSDIFF_FILE_ERROR;
	}
	if (bsdiff_open_substream(packer->stream, read_start, read_end, &(packer->epf)) != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;
	if (bsdiff_create_bz2_decompressor(&(packer->epf_dec)) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	if (packer->epf_dec.init(packer->epf_dec.state, &(packer->epf)) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;

	packer->new_size = newsize;

	*size = packer->new_size;

	return BSDIFF_SUCCESS;
}
/**
 * @brief get the header_x, header_y, header_z of the Entry_head data
 * 
 * @param state point address of bz2_patch_packer
 * @param diff packer->header_x;
 * @param extra  packer->header_y;
 * @param seek  packer->header_z;
 * @return int 
 */
static int bz2_patch_packer_read_entry_header(
	void *state, int64_t *diff, int64_t *extra, int64_t *seek)
{
	int ret;
	uint8_t buf[24];
	size_t cb;

	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_READ);
	assert(packer->new_size >= 0);
	assert(packer->header_x == 0 && packer->header_y == 0);

	ret = packer->cpf_dec.read(packer->cpf_dec.state, buf, 24, &cb);
	if ((ret != BSDIFF_SUCCESS && ret != BSDIFF_END_OF_FILE) || (cb != 24))
		return BSDIFF_ERROR;
	packer->header_x = offtin(buf);
	packer->header_y = offtin(buf + 8);
	packer->header_z = offtin(buf + 16);

	*diff  = packer->header_x;
	*extra = packer->header_y;
	*seek  = packer->header_z;

	return BSDIFF_SUCCESS;
}
/**
 * @brief read_entry_diff from packer->dpf_dec.state and save to buffer
 * 
 * @param state point address of bz2_patch_packer
 * @param buffer store diff block content
 * @param size the size of diff block size
 * @param readed 
 * @return int 
 */
static int bz2_patch_packer_read_entry_diff(
	void *state, void *buffer, size_t size, size_t *readed)
{
	int ret;
	int64_t cb;

	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_READ);
	assert(packer->new_size >= 0);
	assert(packer->header_x >= 0);

	*readed = 0;

	cb = (int64_t)size;
	if (packer->header_x < cb)
		cb = packer->header_x;
	if (cb <= 0)
		return BSDIFF_END_OF_FILE;

	ret = packer->dpf_dec.read(packer->dpf_dec.state, buffer, (size_t)cb, readed);
	packer->header_x -= (int64_t)(*readed);
	return ret;
}
/**
 * @brief read data from packer->epf_dec.state, and save to buffer
 * 
 * @param state point address of bz2_patch_packer
 * @param buffer store extra block content
 * @param size size of extra block content
 * @param readed 
 * @return int 
 */
static int bz2_patch_packer_read_entry_extra(
	void *state, void *buffer, size_t size, size_t *readed)
{
	int ret;
	int64_t cb;

	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_READ);
	assert(packer->new_size >= 0);
	assert(packer->header_y >= 0);

	*readed = 0;

	cb = (int64_t)size;
	if (packer->header_y < cb)
		cb = packer->header_y;
	if (cb <= 0)
		return BSDIFF_END_OF_FILE;

	ret = packer->epf_dec.read(packer->epf_dec.state, buffer, (size_t)cb, readed);
	packer->header_y -= (int64_t)(*readed);
	return ret;
}
/**
 * @brief write header, Initialize compressor for control block, Allocate memory for db && eb
 * 
 * @param state point address of bz2_patch_packer
 * @param size packer->new_size, new file size
 * @return int 
 */
static int bz2_patch_packer_write_new_size(
	void *state, int64_t size)
{
	uint8_t header[32] = { 0 };
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_WRITE);
	assert(packer->new_size == -1);
	assert(size >= 0);

	/* Write a pseudo header */
	if (packer->stream->write(packer->stream->state, header, 32) != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;

	/* Initialize compressor for control block */
	if ((bsdiff_create_bz2_compressor(&(packer->enc)) != BSDIFF_SUCCESS) ||
		(packer->enc.init(packer->enc.state, packer->stream) != BSDIFF_SUCCESS))
	{
		return BSDIFF_ERROR;
	}

	/* Allocate memory for db && eb */
	assert(packer->db == NULL && packer->dblen == 0);
	assert(packer->eb == NULL && packer->eblen == 0);
	packer->db = malloc((size_t)(size + 1));
	packer->eb = malloc((size_t)(size + 1));
	if (!packer->db || !packer->eb)
		return BSDIFF_OUT_OF_MEMORY;
	packer->dblen = 0;
	packer->eblen = 0;

	packer->new_size = size;

	return BSDIFF_SUCCESS;
}
/**
 * @brief write head and compress
 * 
 * @param state point address of bz2_patch_packer
 * @param diff diff size
 * @param extra  extra size
 * @param seek  seek size
 * @return int 
 */
static int bz2_patch_packer_write_entry_header(
	void *state, int64_t diff, int64_t extra, int64_t seek)
{
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_WRITE);
	assert(packer->new_size >= 0);
	assert(diff >= 0);
	assert(extra >= 0);

	assert(packer->header_x == 0 && packer->header_y == 0);
	packer->header_x = diff;
	packer->header_y = extra;
	packer->header_z = seek;

	/* Write a triple */
	uint8_t buf[24];
	offtout(packer->header_x, buf);
	offtout(packer->header_y, buf + 8);
	offtout(packer->header_z, buf + 16);
	int ret = packer->enc.write(packer->enc.state, buf, 24);
	if (ret != BSDIFF_SUCCESS)
		return ret;

	return BSDIFF_SUCCESS;
}
/**
 * @brief 
 * 
 * @param state point address of bsdiff_patch_packer 
 * @param buffer 
 * @param size 
 * @return int 
 */
static int bz2_patch_packer_write_entry_diff(
	void *state, const void *buffer, size_t size)
{
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_WRITE);
	assert(packer->new_size >= 0);

	if ((int64_t)size > packer->header_x)
		return BSDIFF_INVALID_ARG;
	if (packer->dblen + (int64_t)size > packer->new_size)
		return BSDIFF_INVALID_ARG;
	memcpy(packer->db + packer->dblen, buffer, size);
	packer->dblen += (int64_t)size;
	packer->header_x -= (int64_t)size;

	return BSDIFF_SUCCESS;
}

static int bz2_patch_packer_write_entry_extra(
	void *state, const void *buffer, size_t size)
{
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_WRITE);
	assert(packer->new_size >= 0);

	if ((int64_t)size > packer->header_y)
		return BSDIFF_INVALID_ARG;
	if (packer->eblen + (int64_t)size > packer->new_size)
		return BSDIFF_INVALID_ARG;
	memcpy(packer->eb + packer->eblen, buffer, size);
	packer->eblen += (int64_t)size;
	packer->header_y -= (int64_t)size;

	return BSDIFF_SUCCESS;
}

static int bz2_patch_packer_flush(void *state)
{
	uint8_t header[32] = { 0 };
	int64_t patchsize, patchsize2;
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	assert(packer->mode == BSDIFF_MODE_WRITE);
	assert(packer->new_size >= 0);
	assert(packer->header_x == 0 && packer->header_y == 0);

	memcpy(header, "BSDIFF40", 8);
	offtout(packer->new_size, header + 24);

	/* Flush ctrl data */
	if (packer->enc.flush(packer->enc.state) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	//bsdiff_close_compressor(&(packer->enc));

	/* Compute size of compressed ctrl data */
	if (packer->stream->tell(packer->stream->state, &patchsize) != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;
	offtout(patchsize - 32, header + 8);

	/* Write compressed diff data */
	if ((bsdiff_create_bz2_compressor(&(packer->enc)) != BSDIFF_SUCCESS) ||
		(packer->enc.init(packer->enc.state, packer->stream) != BSDIFF_SUCCESS))
	{
		return BSDIFF_ERROR;
	}
	if (packer->enc.write(packer->enc.state, packer->db, (size_t)packer->dblen) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	if (packer->enc.flush(packer->enc.state) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	//bsdiff_close_compressor(&(packer->enc));

	/* Compute size of compressed diff data */
	if (packer->stream->tell(packer->stream->state, &patchsize2) != BSDIFF_SUCCESS)
		return BSDIFF_FILE_ERROR;
	offtout(patchsize2 - patchsize, header + 16);

	/* Write compressed extra data */
	if ((bsdiff_create_bz2_compressor(&(packer->enc)) != BSDIFF_SUCCESS) ||
		(packer->enc.init(packer->enc.state, packer->stream) != BSDIFF_SUCCESS))
	{
		return BSDIFF_ERROR;
	}
	if (packer->enc.write(packer->enc.state, packer->eb, (size_t)packer->eblen) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	if (packer->enc.flush(packer->enc.state) != BSDIFF_SUCCESS)
		return BSDIFF_ERROR;
	//packer->stream->state 0x0000019c09cfdf00  packer->enc.state  0x0000019c09d637b0  newsize(210)  415920(Base 10)
	// db(204)  0x0000019c09d63590  eb(6) 0x0000019c09d636a0
	//header(32) 0x0000007c119af258 "BSDIFF40C"
	fprintf(stderr, "packer->enc.state file: %s , size = %d \n", packer->enc.state, strlen(packer->enc.state));
	//bsdiff_close_compressor(&(packer->enc));

	/* Seek to the beginning, (re)write the header */
	if ((packer->stream->seek(packer->stream->state, 0, BSDIFF_SEEK_SET) != BSDIFF_SUCCESS) ||
		(packer->stream->write(packer->stream->state, header, 32) != BSDIFF_SUCCESS) ||
		(packer->stream->flush(packer->stream->state) != BSDIFF_SUCCESS))
	{
		return BSDIFF_FILE_ERROR;
	}
	fprintf(stderr, "packer->stream->state file: %s , size = %d \n", packer->stream->state, strlen(packer->stream->state));
	return BSDIFF_SUCCESS;
}

static void bz2_patch_packer_close(void *state)
{
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	
	if (packer->mode == BSDIFF_MODE_READ) {
		bsdiff_close_decompressor(&(packer->cpf_dec));
		bsdiff_close_decompressor(&(packer->dpf_dec));
		bsdiff_close_decompressor(&(packer->epf_dec));
		bsdiff_close_stream(&(packer->cpf));
		bsdiff_close_stream(&(packer->dpf));
		bsdiff_close_stream(&(packer->epf));
	} else {
		bsdiff_close_compressor(&(packer->enc));
		free(packer->db);
		free(packer->eb);
	}

	bsdiff_close_stream(packer->stream);

	free(packer);
}

static int bz2_patch_packer_getmode(void *state)
{
	struct bz2_patch_packer *packer = (struct bz2_patch_packer*)state;
	return packer->mode;
}
/**
 * @brief set the mode of bsdiff_patch_packer, and reset the operation functions piont address
 * 
 * @param bsdiff_packer point address of bsdiff_patch_packer
 * @param mode 
 *   BSDIFF_MODE_READ  0
 *   BSDIFF_MODE_WRITE 1
 * @return int bz2_packer->mode
 */
static int bz2_patch_packer_setmode(void* bsdiff_packer, int mode)
{
	struct bsdiff_patch_packer* packer = (struct bsdiff_patch_packer*)bsdiff_packer;
	struct bz2_patch_packer* bz2_packer = (struct bz2_patch_packer*)packer->state;
	struct bsdiff_stream* bstream = (struct bsdiff_stream*)bz2_packer->stream;
	bz2_packer->mode = mode;
	bstream->set_mode(bstream, mode);
	if (mode == BSDIFF_MODE_READ) {
		packer->read_new_size = bz2_patch_packer_read_new_size;
		packer->read_entry_header = bz2_patch_packer_read_entry_header;
		packer->read_entry_diff = bz2_patch_packer_read_entry_diff;
		packer->read_entry_extra = bz2_patch_packer_read_entry_extra;
	}
	else {
		packer->write_new_size = bz2_patch_packer_write_new_size;
		packer->write_entry_header = bz2_patch_packer_write_entry_header;
		packer->write_entry_diff = bz2_patch_packer_write_entry_diff;
		packer->write_entry_extra = bz2_patch_packer_write_entry_extra;
		packer->flush = bz2_patch_packer_flush;
	}
	return bz2_packer->mode;
}
/**
 * @brief create bz2_patch_packer
 * 
 * @param mode 
 *   BSDIFF_MODE_READ  0
 *   BSDIFF_MODE_WRITE 1
 * @param stream  bsdiff_stream
 * @param packer bsdiff_patch_packer
 * @return int 
 */
int bsdiff_open_bz2_patch_packer(
	int mode,
	struct bsdiff_stream *stream,
	struct bsdiff_patch_packer *packer)
{
	struct bz2_patch_packer *state;
	assert(mode >= BSDIFF_MODE_READ && mode <= BSDIFF_MODE_WRITE);
	assert(stream);
	assert(packer);

	state = malloc(sizeof(struct bz2_patch_packer));
	if (!state)
		return BSDIFF_OUT_OF_MEMORY;
	memset(state, 0, sizeof(*state));
	state->stream = stream;
	state->mode = mode;
	state->new_size = -1;

	memset(packer, 0, sizeof(*packer));
	packer->state = state;
	if (mode == BSDIFF_MODE_READ) {
		packer->read_new_size      = bz2_patch_packer_read_new_size;
		packer->read_entry_header  = bz2_patch_packer_read_entry_header;
		packer->read_entry_diff    = bz2_patch_packer_read_entry_diff;
		packer->read_entry_extra   = bz2_patch_packer_read_entry_extra;
	} else {
		packer->write_new_size     = bz2_patch_packer_write_new_size;
		packer->write_entry_header = bz2_patch_packer_write_entry_header;
		packer->write_entry_diff   = bz2_patch_packer_write_entry_diff;
		packer->write_entry_extra  = bz2_patch_packer_write_entry_extra;
		packer->flush              = bz2_patch_packer_flush;
	}
	packer->close = bz2_patch_packer_close;
	packer->get_mode = bz2_patch_packer_getmode;
	packer->set_mode = bz2_patch_packer_setmode;
	
	return BSDIFF_SUCCESS;
}


