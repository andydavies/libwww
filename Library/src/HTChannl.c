/*								     HTChannl.c
**	CONTAINS STREAMS FOR READING AND WRITING TO AND FROM A TRANSPORT
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**	@(#) $Id$
**
**
** HISTORY:
**	April 96  HFN	Written
*/

/* Library Include files */
#include "sysdep.h"
#include "WWWUtil.h"
#include "HTAlert.h"
#include "HTHost.h"
#include "HTError.h"
#include "HTChannl.h"					 /* Implemented here */

#define HASH_SIZE	67
#define HASH(s)		((s) % HASH_SIZE)

struct _HTInputStream {
    const HTInputStreamClass *	isa;
    HTChannel *			channel;
};

struct _HTOutputStream {
    const HTOutputStreamClass *	isa;
    HTChannel *			channel;
};

struct _HTChannel {
    /* what media do we talk to? */
    SOCKET		sockfd;					   /* Socket */
    FILE *		fp;

    /* what streams handle the IO */
    HTInputStream *	input;				     /* Input stream */
    HTOutputStream *	output;				    /* Output stream */
    /* proxy streams to dereference the above streams */
    HTInputStream	channelIStream;
    HTOutputStream	channelOStream;

    BOOL		active;			/* Active or passive channel */
    int			semaphore;			   /* On channel use */
    HTHost *		host;			       /* Zombie connections */
};

PRIVATE HTList	** channels = NULL;			 /* List of channels */

/* ------------------------------------------------------------------------- */

/*
**	Skinny stream objects to pass the IO requests to the channels current IO streams.
**	This was needed because the channel's IO streams could go away after the IO streams
**	were set up for multiple requests.
*/

PRIVATE int ChannelIStream_flush (HTInputStream * me)
{return me->channel->input ? (*me->channel->input->isa->flush)(me->channel->input) : HT_ERROR;}
PRIVATE int ChannelIStream_free (HTInputStream * me)
{return me->channel->input ? (*me->channel->input->isa->_free)(me->channel->input) : HT_ERROR;}
PRIVATE int ChannelIStream_abort (HTInputStream * me, HTList * e)
{return me->channel->input ? (*me->channel->input->isa->abort)(me->channel->input, e) : HT_ERROR;}
PRIVATE int ChannelIStream_read (HTInputStream * me)
{return me->channel->input ? (*me->channel->input->isa->read)(me->channel->input) : HT_ERROR;}
PRIVATE int ChannelIStream_close (HTInputStream * me)
{return me->channel->input ? (*me->channel->input->isa->close)(me->channel->input) : HT_ERROR;}
PUBLIC int ChannelIStream_consumed (HTInputStream * me, size_t bytes)
{return me->channel->input ? (*me->channel->input->isa->consumed)(me->channel->input, bytes) : HT_ERROR;}
PRIVATE const HTInputStreamClass ChannelIStreamIsa =
{
    "ChannelInput",
    ChannelIStream_flush,
    ChannelIStream_free,
    ChannelIStream_abort,
    ChannelIStream_read,
    ChannelIStream_close,
    ChannelIStream_consumed
}; 

PRIVATE int ChannelOStream_flush (HTOutputStream * me)
{return me->channel->output ? (*me->channel->output->isa->flush)(me->channel->output) : HT_ERROR;}
PRIVATE int ChannelOStream_free (HTOutputStream * me)
{return me->channel->output ? (*me->channel->output->isa->_free)(me->channel->output) : HT_ERROR;}
PRIVATE int ChannelOStream_abort (HTOutputStream * me, HTList * e)
{return me->channel->output ? (*me->channel->output->isa->abort)(me->channel->output, e) : HT_ERROR;}
PRIVATE int ChannelOStream_put_character (HTOutputStream * me, char c)
{return me->channel->output ? (*me->channel->output->isa->put_character)(me->channel->output, c) : HT_ERROR;}
PRIVATE int ChannelOStream_put_string (HTOutputStream * me, const char * s)
{return me->channel->output ? (*me->channel->output->isa->put_string)(me->channel->output, s) : HT_ERROR;}
PRIVATE int ChannelOStream_put_block (HTOutputStream * me, const char * buf, int len)
{return me->channel->output ? (*me->channel->output->isa->put_block)(me->channel->output, buf, len) : HT_ERROR;}
PRIVATE int ChannelOStream_close (HTOutputStream * me)
{return me->channel->output ? (*me->channel->output->isa->close)(me->channel->output) : HT_ERROR;}
PRIVATE const HTOutputStreamClass ChannelOStreamIsa =
{
    "ChannelOutput",
    ChannelOStream_flush,
    ChannelOStream_free,
    ChannelOStream_abort,
    ChannelOStream_put_character,
    ChannelOStream_put_string,
    ChannelOStream_put_block,
    ChannelOStream_close,
}; 

/* ------------------------------------------------------------------------- */

PRIVATE void free_channel (HTChannel * ch)
{
    if (ch) {

	/* Close the input and output stream */
	if (ch->input) {
	    (*ch->input->isa->close)(ch->input);
	    ch->input = NULL;
	}
	if (ch->output) {
	    (*ch->output->isa->close)(ch->output);
	    ch->output = NULL;
	}

	/* Close the socket */
	if (ch->sockfd != INVSOC) {
	    NETCLOSE(ch->sockfd);
	    /*	    HTEvent_unregister(ch->sockfd, all options); */
   	    HTNet_decreaseSocket();
	    if (PROT_TRACE)
		HTTrace("Channel..... Deleted %p, socket %d\n", ch,ch->sockfd);
	}
	ch->sockfd = INVSOC;

	/* Close the file */
	if (ch->fp) {
	    fclose(ch->fp);
	    if (PROT_TRACE)
		HTTrace("Channel..... Deleted %p, file %p\n", ch, ch->fp);
	}
	HT_FREE(ch);
    }
}

/*
**	A channel is uniquely identified by a socket.
**	Note that we don't create the input and output stream - they are 
**	created later.
**
**	We only keep a hash on sockfd's as we don't have to look for channels
**	for ANSI file descriptors.
*/
PUBLIC HTChannel * HTChannel_new (SOCKET sockfd, BOOL active)
{
    HTList * list = NULL;
    HTChannel * ch = NULL;
    int hash = sockfd < 0 ? 0 : HASH(sockfd);
    if (PROT_TRACE) HTTrace("Channel..... Hash value is %d\n", hash);
    if (!channels) {
	if (!(channels = (HTList **) HT_CALLOC(HASH_SIZE,sizeof(HTList*))))
	    HT_OUTOFMEM("HTChannel_new");
    }
    if (!channels[hash]) channels[hash] = HTList_new();
    list = channels[hash];
    if ((ch = (HTChannel *) HT_CALLOC(1, sizeof(HTChannel))) == NULL)
	HT_OUTOFMEM("HTChannel_new");	    
    ch->sockfd = sockfd;
    ch->active = active;
    ch->semaphore = 1;
    ch->channelIStream.isa = &ChannelIStreamIsa;
    ch->channelOStream.isa = &ChannelOStreamIsa;
    ch->channelIStream.channel = ch;
    ch->channelOStream.channel = ch;
    HTList_addObject(list, (void *) ch);

    if (PROT_TRACE) HTTrace("Channel..... Added %p to list %p\n", ch,list);
    return ch;
}

/*
**	Look for a channel object if we for some reason should have lost it
**	Returns NULL if nothing found
*/
PUBLIC HTChannel * HTChannel_find (SOCKET sockfd)
{
    if (channels && sockfd != INVSOC) {
	int hash = HASH(sockfd);
	HTList * list = channels[hash];
	if (list) {
	    HTChannel * ch = NULL;
	    while ((ch = (HTChannel *) HTList_nextObject(list)))
		if (ch->sockfd == sockfd) return ch;
	}
    }
    return NULL;
}

/*
**	When deleting a channel we first look at if there are no more requests
**	using the channel (the semaphore is <= 0). Then, if the socket supports
**	persistent connections then we register the channel in the Host cache
**	and wait until the other end closes it or we get a time out on our side
*/
PUBLIC BOOL HTChannel_delete (HTChannel * channel, int status)
{
    if (channel) {
	if (PROT_TRACE) HTTrace("Channel..... Delete %p with semaphore %d\n",
				channel, channel->semaphore);
	/*
	**  We call the free methods on both the input stream and the output
	**  stream so that we can free up the stream pipes. However, note that
	**  this doesn't mean that we close the input stream and output stream
	**  them selves - only the generic streams
	*/
	if (status != HT_IGNORE) {
	    if (channel->input) {
		if (status == HT_INTERRUPTED)
		    (*channel->input->isa->abort)(channel->input, NULL);
		else
		    (*channel->input->isa->_free)(channel->input);
	    }
	    if (channel->output) {
		if (status == HT_INTERRUPTED)
		    (*channel->output->isa->abort)(channel->output, NULL);
		else
		    (*channel->output->isa->_free)(channel->output);
	    }
	}

	/*
	**  Check whether this channel is used by other objects or we can
	**  delete it and free memory.
	*/
	if (channel->semaphore <= 0 && channels && channel->sockfd != INVSOC) {
	    int hash = HASH(channel->sockfd);
	    HTList * list = channels[hash];
	    if (list) {
		HTList_removeObject(list, (void *) channel);
		free_channel(channel);
		return YES;
	    }
	} else
	    HTChannel_downSemaphore(channel);
    }
    return NO;
}

/*	HTChannel_deleteAll
**	-------------------
**	Destroys all channels. This is called by HTLibTerminate(0
*/
PUBLIC BOOL HTChannel_deleteAll (void)
{
    if (channels) {
	HTList * cur;
	int cnt;
	for (cnt=0; cnt<HASH_SIZE; cnt++) {
	    if ((cur = channels[cnt])) { 
		HTChannel * pres;
		while ((pres = (HTChannel *) HTList_nextObject(cur)) != NULL)
		    free_channel(pres);
	    }
	    HTList_delete(channels[cnt]);
	}
	HT_FREE(channels);
    }
    return YES;
}

/*
**	Return the socket associated with this channel
*/
PUBLIC SOCKET HTChannel_socket (HTChannel * channel)
{
    return channel ? channel->sockfd : INVSOC;
}

PUBLIC void HTChannel_setSocket (HTChannel * channel, SOCKET socket)
{
    if (channel)
      channel->sockfd = socket;
}

/*
**	Return the file descriptor associated with this channel
*/
PUBLIC FILE * HTChannel_file (HTChannel * channel)
{
    return channel ? channel->fp : NULL;
}

/*
**	We keep the associated Host object in case we have a
**	sleeping connection. 
*/
PUBLIC BOOL HTChannel_setHost (HTChannel * ch, HTHost * host)
{
    if (ch) {
	ch->host = host;
	return YES;
    }
    return NO;
}

PUBLIC HTHost * HTChannel_host (HTChannel * ch)
{
    return (ch ? ch->host : NULL);
}

/*
**	Increase the semaphore for this channel
*/
PUBLIC void HTChannel_upSemaphore (HTChannel * channel)
{
    if (channel) {
	channel->semaphore++;
	if (PROT_TRACE)
	    HTTrace("Channel..... Semaphore increased to %d for channel %p\n",
		    channel->semaphore, channel);
    }
}

/*
**	Decrease the semaphore for this channel
*/
PUBLIC void HTChannel_downSemaphore (HTChannel * channel)
{
    if (channel) {
	channel->semaphore--;
	if (channel->semaphore <= 0) channel->semaphore = 0;
	if (PROT_TRACE)
	    HTTrace("Channel..... Semaphore decreased to %d for channel %p\n",
		    channel->semaphore, channel);
    }
}

/*
**	Explicitly set the semaphore for this channel
*/
PUBLIC void HTChannel_setSemaphore (HTChannel * channel, int semaphore)
{
    if (channel) {
	channel->semaphore = semaphore;
	if (channel->semaphore <= 0) channel->semaphore = 0;
	if (PROT_TRACE)
	    HTTrace("Channel..... Semaphore set to %d for channel %p\n",
		    channel->semaphore, channel);
    }
}

/*
**	Create the input stream and bind it to the channel
**	Please read the description in the HTIOStream module on the parameters
*/
PUBLIC BOOL HTChannel_setInput (HTChannel * ch, HTInputStream * input)
{
    if (ch) {
	ch->input = input;
	return YES;
    }
    return NO;
}

PUBLIC HTInputStream * HTChannel_input (HTChannel * ch)
{
    return ch ? ch->input : NULL;
}

/*
**	Create the output stream and bind it to the channel
**	Please read the description in the HTIOStream module on the parameters
*/
PUBLIC BOOL HTChannel_setOutput (HTChannel * ch, HTOutputStream * output)
{
    if (ch) {
	ch->output = output;
	return YES;
    }
    return NO;
}

PUBLIC HTOutputStream * HTChannel_output (HTChannel * ch)
{
    return ch ? ch->output : NULL;
}

PUBLIC HTInputStream * HTChannel_getChannelIStream (HTChannel * ch)
{
    if (ch)
	return &ch->channelIStream;
    return NULL;
}

PUBLIC HTOutputStream * HTChannel_getChannelOStream (HTChannel * ch)
{
    if (ch)
	return &ch->channelOStream;
    return NULL;
}

