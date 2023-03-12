#include "bsdiff.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/**
 * @brief memstream structure
 * @param: mode: operation type
 * @param: buffer: the memory buffer
 * @param: size: size of the memory buffer
 * @param: capacity: the allocate memory buffer
 * @param: pos: the operate position of the memory buffer
 */
struct memstream_state
{
	int mode;
	void *buffer;
	size_t size;
	size_t capacity;
	size_t pos;
};
/**
 * @brief set the s->pos according to origin type
 * 
 * @param state point address of the memstream_state
 * @param offset memory offset
 * @param origin the type to decide the base position of offset
 * 	BSDIFF_SEEK_SET: 0
 *  BSDIFF_SEEK_CUR: s->pos
 *  BSDIFF_SEEK_END: s->end
 * @return int 
 */
static int memstream_seek(void *state, int64_t offset, int origin)
{
	struct memstream_state *s = (struct memstream_state*)state;
	int64_t newpos = -1;

	switch (origin) {
	case BSDIFF_SEEK_SET:
		newpos = offset;
		break;
	case BSDIFF_SEEK_CUR:
		newpos = (int64_t)s->pos + offset;
		break;
	case BSDIFF_SEEK_END:
		newpos = (int64_t)s->size + offset;
		break;
	}
	if (newpos < 0 || newpos > (int64_t)s->size)
		return BSDIFF_INVALID_ARG;

	s->pos = (size_t)newpos;

	return BSDIFF_SUCCESS;
}
/**
 * @brief get the operation postion pos of memery buffer
 * 
 * @param state point address of the memstream_state
 * @param position the pos to save
 * @return int 
 */
static int memstream_tell(void *state, int64_t *position)
{
	struct memstream_state *s = (struct memstream_state*)state;
	*position = (int64_t)s->pos;
	return BSDIFF_SUCCESS;
}
/**
 * @brief wite buffer in memstream_state to buffer
 * 
 * @param state point address of the memstream_state
 * @param buffer buffer to save the memstream buffer
 * @param size read size
 * @param readed 
 * @return int 
 */
static int memstream_read(void *state, void *buffer, size_t size, size_t *readed)
{
	struct memstream_state *s = (struct memstream_state*)state;
	size_t cb;

	assert(s->mode == BSDIFF_MODE_READ);

	*readed = 0;
	
	if (size == 0)
		return BSDIFF_SUCCESS;

	cb = size;
	if (s->pos + size > s->size)
		cb = s->size - s->pos;

	memcpy(buffer, (uint8_t*)s->buffer + s->pos, cb);

	s->pos += cb;

	return (cb < size) ? BSDIFF_END_OF_FILE : BSDIFF_SUCCESS;
}
/**
 * @brief recalculate the buffer size according to required size
 * 
 * @param current buffer size
 * @param required required buffer size
 * @return size_t buffer size after calculate
 */
static size_t calc_new_capacity(size_t current, size_t required)
{
	size_t cap = current;
	while (cap < required) {
		/*
		  https://github.com/facebook/folly/blob/main/folly/docs/FBVector.md#memory-handling
		  Our strategy: empty() ? 4096 : capacity() * 1.5
		 */
		if (cap == 0)
			cap = 4096;
		else
			cap = (cap * 3 + 1) / 2;
	}
	return cap;
}

/**
 * @brief write the buffer to memstream_state buffer area,
 *  address of the area is (uint8_t*)s->buffer + s->pos, wite size is size;
 * 
 * @param state point address of the memstream_state
 * @param buffer buffer that wait to write to memory
 * @param size buffer size to write
 * @return int 
 */
static int memstream_write(void *state, const void *buffer, size_t size)
{
	struct memstream_state *s = (struct memstream_state*)state;
	void *newbuf;
	size_t newcap;

	assert(s->mode == BSDIFF_MODE_WRITE);

	if (size == 0)
		return BSDIFF_SUCCESS;

	/* Grow the capacity if needed */
	if (s->pos + size > s->capacity) {
		newcap = calc_new_capacity(s->capacity, s->pos + size);

		newbuf = realloc(s->buffer, newcap);
		if (!newbuf)
			return BSDIFF_OUT_OF_MEMORY;

		s->buffer = newbuf;
		s->capacity = newcap;
	}

	/* memcpy */
	memcpy((uint8_t*)s->buffer + s->pos, buffer, size);

	/* Update pos */
	s->pos += size;

	/* Update size */
	if (s->pos > s->size)
		s->size = s->pos;

	return BSDIFF_SUCCESS;
}
/**
 * @brief todo
 * 
 * @param state point address of the memstream_state
 * @return int 
 */
static int memstream_flush(void *state)
{
	struct memstream_state *s = (struct memstream_state*)state;

	assert(s->mode == BSDIFF_MODE_WRITE);
	//(void)s;
	//todo? not understand this

	return BSDIFF_SUCCESS;
}
/**
 * @brief Function: set the memberBuffer to *ppbuffer, and buffer size to psize
 * 
 * @param state point address of the memstream_state
 * @param ppbuffer 
 * @param psize member buffer size
 * @return int BSDIFF_SUCCESS
 */
static int memstream_getbuffer(void *state, const void **ppbuffer, size_t *psize)
{
	struct memstream_state *s = (struct memstream_state*)state;
	fprintf(stderr, " memstream_getbuffer patch packer %s, size = %d\n", s->buffer, s->size);
	//void* newbuf = malloc(s->size);
    /* memcpy */
    //memcpy((uint8_t*)newbuf, s->buffer, s->size);
	//fprintf(stderr, " memstream_getbuffer patch packer %s, size = %d\n", newbuf, s->size);
	*ppbuffer = s->buffer;
	*psize = s->size;

	return BSDIFF_SUCCESS;
}
/**
 * @brief Function: free buffer memery and memstream_state
 * 
 * @param state point address of the memstream_state
 */
static void memstream_close(void *state)
{
	struct memstream_state *s = (struct memstream_state*)state;

	if (s->mode == BSDIFF_MODE_WRITE) {
		free(s->buffer);
	}

	free(s);
}

/**
 * @brief Function: get the model of memStream
 * 
 * @param state point address of memstream_state
 * @return int 
 * 	 BSDIFF_MODE_READ  0
 *   BSDIFF_MODE_WRITE 1
 */
static int memstream_getmode(void *state)
{
	struct memstream_state *s = (struct memstream_state*)state;
	return s->mode;
}
/**
 * @brief Function: get the model of memStream
 *
 * @param state point address of bsdiff_stream
 * @mode: operate mode
 * @return int mode
 * 	 BSDIFF_MODE_READ  0
 *   BSDIFF_MODE_WRITE 1
 */
static int memstream_setmode(void* bsdiff_str, int mode)
{
	struct bsdiff_stream* stream = (struct bsdiff_stream*)bsdiff_str;
	struct memstream_state* state = (struct memstream_state*)stream->state;
	state->mode = mode;
	if (state->mode == BSDIFF_MODE_READ) {
		stream->read = memstream_read;
	}
	else {
		stream->write = memstream_write;
		stream->flush = memstream_flush;
	}
	return state->mode;
}
/**
 * @brief 
 * 
 * @param mode memoperate type
 * @param buffer  memery buffer address point, if writeMode, set to NULL
 * @param size memery size
 * @param stream patchfile stream structure
 * @return int  BSDIFF_SUCCESS: open memory_stream OK
 */
int bsdiff_open_memory_stream(
	int mode,
	const void *buffer, 
	size_t size,
	struct bsdiff_stream *stream)
{
	struct memstream_state *state;
	assert(mode >= BSDIFF_MODE_READ && mode <= BSDIFF_MODE_WRITE);
	assert(stream);

	state = malloc(sizeof(struct memstream_state));
	if (state == NULL)
		return BSDIFF_OUT_OF_MEMORY;

	if (mode == BSDIFF_MODE_READ) {
		/* read mode */
		if (buffer == NULL) {
			return BSDIFF_INVALID_ARG;
		}
		state->mode = BSDIFF_MODE_READ;
		state->buffer = (void*)buffer;
		state->capacity = size;
		state->size = size;
	} else {
		/* write mode */
		if (buffer != NULL) {
			return BSDIFF_INVALID_ARG;
		}
		state->mode = BSDIFF_MODE_WRITE;
		state->size = 0;
		if (size > 0) {
			/* initial reservation */
			state->buffer = malloc(size);
			if (state->buffer == NULL) {
				free(state);
				return BSDIFF_OUT_OF_MEMORY;
			}
			state->capacity = size;
		} else {
			state->buffer = NULL;
			state->capacity = 0;
		}
	}
	state->pos = 0;

	memset(stream, 0, sizeof(*stream));
	stream->state = state;
	stream->close = memstream_close;
	stream->get_mode = memstream_getmode;
	stream->set_mode = memstream_setmode;
	stream->seek = memstream_seek;
	stream->tell = memstream_tell;
	if (state->mode == BSDIFF_MODE_READ) {
		stream->read = memstream_read;
	} else {
		stream->write = memstream_write;
		stream->flush = memstream_flush;
	}
	stream->get_buffer = memstream_getbuffer;

	return BSDIFF_SUCCESS;
}

