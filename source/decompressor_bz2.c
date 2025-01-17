#include "bsdiff.h"
#include "bsdiff_private.h"
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>

struct bz2_decompressor
{
	/*flag of initialized, 1: initialized, 0: not initialized */
	int initialized;
	/*bsdiff_stream structure*/
	struct bsdiff_stream *strm;
	/*bz_stream*/
	bz_stream bzstrm;  
	int bzerr;
	/*buffer to temperally save data, if full, wite to outer buffer*/
	char buf[5000];
};
/**
 * @brief 
 * 
 * @param state point address of bz2_decompressor
 * @param stream point address of bsdiff_stream
 * @return int 
 * 	BSDIFF_ERROR: failed
 * 	BSDIFF_SUCCESS: ok
 */
static int bz2_decompressor_init(void *state, struct bsdiff_stream *stream)
{
	struct bz2_decompressor *dec = (struct bz2_decompressor*)state;

	if (dec->initialized)
		return BSDIFF_ERROR;

	dec->strm = stream;

	dec->bzstrm.bzalloc = NULL;
	dec->bzstrm.bzfree = NULL;
	dec->bzstrm.opaque = NULL;
	if (BZ2_bzDecompressInit(&(dec->bzstrm), 0, 0) != BZ_OK)
		return BSDIFF_ERROR;
	dec->bzstrm.avail_in = 0;
	dec->bzstrm.next_in = NULL;
	dec->bzstrm.avail_out = 0;
	dec->bzstrm.next_out = NULL;
	
	dec->bzerr = BZ_OK;

	dec->initialized = 1;

	return BSDIFF_SUCCESS;
}
/**
 * @brief bz_decompress data to buffer
 * 
 * @param state point address of bz2_decompressor
 * @param buffer memery buffer store the decompressed data
 * @param size  size of required read data
 * @param readed size of readed data
 * @return int 
 * 	BSDIFF_SUCCESS: OK
 *  BSDIFF_ERROR: error
 *  BSDIFF_END_OF_FILE
 */
static int bz2_decompressor_read(void *state, void *buffer, size_t size, size_t *readed)
{
	struct bz2_decompressor *dec = (struct bz2_decompressor*)state;
	int ret;
	size_t cb;
	unsigned int old_avail_out;

	*readed = 0;

	if (!dec->initialized)
		return BSDIFF_ERROR;
	if (dec->bzerr != BZ_OK)
		return (dec->bzerr == BZ_STREAM_END) ? BSDIFF_END_OF_FILE : BSDIFF_ERROR;
	if (size >= UINT32_MAX)
		return BSDIFF_INVALID_ARG;
	if (size == 0)
		return BSDIFF_SUCCESS;

	dec->bzstrm.avail_out = (unsigned int)size;
	dec->bzstrm.next_out = (char*)buffer;

	while (1) {
		/* input buffer is empty */
		if (dec->bzstrm.avail_in == 0) {
			ret = dec->strm->read(dec->strm->state, dec->buf, sizeof(dec->buf), &cb);
			if ((ret != BSDIFF_SUCCESS && ret != BSDIFF_END_OF_FILE) || (cb == 0))
				return BSDIFF_ERROR;
			dec->bzstrm.next_in = dec->buf;
			dec->bzstrm.avail_in = (unsigned int)cb;
		}

		old_avail_out = dec->bzstrm.avail_out;
		/* decompress some amount of data */
		dec->bzerr = BZ2_bzDecompress(&(dec->bzstrm));
		if (dec->bzerr != BZ_OK && dec->bzerr != BZ_STREAM_END)
			return BSDIFF_ERROR;

		/* update readed */
		*readed += old_avail_out - dec->bzstrm.avail_out;

		/* the end of compressed stream was detected */
		if (dec->bzerr == BZ_STREAM_END)
			return BSDIFF_END_OF_FILE;
		/* all output buffer has been consumed */
		if (dec->bzstrm.avail_out == 0)
			return BSDIFF_SUCCESS;
	}

	/* never reached */
	return BSDIFF_ERROR;
}
/**
 * @brief if bz2_decompressor is initialized, clean up BZ2 decompress state, free bz2_decompressor
 * 
 * @param state point address of bz2_decompressor
 *
 */
static void bz2_decompressor_close(void *state)
{
	struct bz2_decompressor *dec = (struct bz2_decompressor*)state;

	if (dec->initialized) {
		/* cleanup BZ2 decompress state */
		BZ2_bzDecompressEnd(&(dec->bzstrm));
	}

	/* free the state */
	free(dec);
}
/**
 * @brief create bsdiff_decompressor structure
 * 
 * @param dec bsdiff_decompressor point address, to be create and initialized
 * 	dec->init = bz2_decompressor_init;
	dec->read = bz2_decompressor_read;
	dec->close = bz2_decompressor_close;
 * @return int 
 */
int bsdiff_create_bz2_decompressor(
	struct bsdiff_decompressor *dec)
{
	struct bz2_decompressor *state;

	state = malloc(sizeof(struct bz2_decompressor));
	if (!state)
		return BSDIFF_OUT_OF_MEMORY;
	state->initialized = 0;
	state->strm = NULL;

	memset(dec, 0, sizeof(*dec));
	dec->state = state;
	dec->init = bz2_decompressor_init;
	dec->read = bz2_decompressor_read;
	dec->close = bz2_decompressor_close;

	return BSDIFF_SUCCESS;
}
