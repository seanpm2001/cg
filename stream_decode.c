#include <stddef.h>

#include "stream.h"
#include "stream_types.h"
#include "util.h"

enum decode_state { DS_OK, DS_ILL, DS_EOF };

struct stream_decode {
    stream st;

    int *table;			/* decoding table to use */

    enum decode_state state;	/* state we're in */
    char *buf;			/* buffer for decoded data */
    int buf_alen;		/* allocation length of buf */
    int rest;			/* partially decoded quadruple */
    int no;			/* number of characters in rest */
};

static int dec_close(struct stream_decode *st);
static token *dec_get(struct stream_decode *st);



stream *
stream_decode_open(struct stream *source, int *table)
{
    struct stream_decode *this;

    this = (struct stream_decode *)stream_new(sizeof(struct stream_decode),
					      dec_get, dec_close, source);

    this->table = table;
    this->rest = this->no = 0;
    this->buf = NULL;
    this->buf_alen = 0;
    this->state = DS_OK;

    return (stream *)this;
}



static int
dec_close(struct stream_decode *this)
{
    /* XXX: skip to EOF? */
      
    stream_free((stream *)this);

    return 0;
}



static token *
dec_get(struct stream_decode *this)
{
    token *t;
    int rest, no, b, i;

    switch (this->state) {
    case DS_ILL:
	/* XXX: error number and text */
	this->state = DS_EOF;
	return token_set(&this->st.tok, TOK_ERR, NULL);

    case DS_EOF:
	return TOKEN_EOF;

    case DS_OK:
	t = stream_get(this->st.source);

	/* XXX: return rest on EOF */

	if (t->type != TOK_LINE)
	    return t;
	
	rest = this->rest;
	no = this->no;
	i = 0;
	
	while (*t->line) {
	    b = this->table[(unsigned char)*(t->line++)];
	    if (b < 0) {
		switch (b) {
		case DEC_END:
		    if (this->state != DS_OK)
			this->state = DS_ILL;
		    else if (no == 0)
			this->state = DS_EOF;
		    else
			this->state = DS_ILL;
		    break;
		    
		case DEC_PAD:
		    if (this->state == DS_OK) {
			switch (no) {
			case 0:
			case 1:
			    this->state = DS_ILL;
			    break;
			case 2:
			case 3:
			    this->state = DS_EOF;
			    break;
			}
		    }
		    break;
		    
		default:
		    /* XXX: recover, don't abort decoding */
		    no = 0;
		    this->state = DS_ILL;
		}
	    }
	    else {
		rest = (rest << 6) | (b & 0x3f);
		no++;
	    }

	    if (no == 4 || this->state != DS_OK) {
		if (i+2 >= this->buf_alen) {
		    this->buf_alen = (this->buf_alen ? this->buf_alen*2 : 64);
		    this->buf = xrealloc(this->buf, this->buf_alen);
		}
		
		switch (no) {
		case 2:
		    this->buf[i++] = rest >> 4;
		    break;

		case 3:
		    this->buf[i++] = rest >> 10;
		    this->buf[i++] = (rest>>2) & 0xff;
		    break;
				         
		case 4:
		    this->buf[i++] = rest >> 16;
		    this->buf[i++] = (rest>>8) & 0xff;
		    this->buf[i++] = (rest & 0xff);
		    break;
		}
		rest = no = 0;
	    }
	}
    }

    this->rest = rest;
    this->no = no;
	
    return token_set3(&this->st.tok, TOK_DATA, i, this->buf);
}