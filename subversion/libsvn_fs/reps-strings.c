/* reps-strings.c : intepreting representations w.r.t. strings
 *
 * ====================================================================
 * Copyright (c) 2000-2003 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <assert.h>
#include <apr_md5.h>
#include <db.h>

#include "svn_fs.h"
#include "svn_pools.h"
#include "svn_md5.h"

#include "fs.h"
#include "err.h"
#include "trail.h"
#include "reps-strings.h"

#include "bdb/reps-table.h"
#include "bdb/strings-table.h"

#include "../libsvn_delta/delta.h"



/*** Helper Functions ***/


/* Return non-zero iff REP is mutable under transaction TXN_ID. */
static int rep_is_mutable (svn_fs__representation_t *rep, const char *txn_id)
{
  if (! rep->txn_id)
    return 0;
  return (! strcmp (rep->txn_id, txn_id));
}


/* Return a `fulltext' representation, allocated in POOL, which
 * references the string STR_KEY.
 * 
 * If TXN_ID is non-zero and non-NULL, make the representation mutable
 * under that TXN_ID.
 * 
 * If STR_KEY is non-null, copy it into an allocation from POOL.
 * 
 * If CHECKSUM is non-null, use it as the checksum for the new rep;
 * else initialize the rep with an all-zero (i.e., always successful)
 * checksum.
 */
static svn_fs__representation_t *
make_fulltext_rep (const char *str_key, 
                   const char *txn_id,
                   const unsigned char *checksum,
                   apr_pool_t *pool)

{
  svn_fs__representation_t *rep = apr_pcalloc (pool, sizeof (*rep));
  if (txn_id && *txn_id)
    rep->txn_id = apr_pstrdup (pool, txn_id);
  rep->kind = svn_fs__rep_kind_fulltext;

  if (checksum)
    memcpy (rep->checksum, checksum, MD5_DIGESTSIZE);
  else
    memset (rep->checksum, 0, MD5_DIGESTSIZE);

  rep->contents.fulltext.string_key 
    = str_key ? apr_pstrdup (pool, str_key) : NULL;
  return rep;
}


/* Set *KEYS to an array of string keys gleaned from `delta'
   representation REP.  Allocate *KEYS in POOL. */
static svn_error_t *
delta_string_keys (apr_array_header_t **keys,
                   const svn_fs__representation_t *rep, 
                   apr_pool_t *pool)
{
  const char *key;
  int i;
  apr_array_header_t *chunks;

  if (rep->kind != svn_fs__rep_kind_delta)
    return svn_error_create 
      (SVN_ERR_FS_GENERAL, NULL,
       "delta_string_key: representation is not of type `delta'");

  /* Set up a convenience variable. */
  chunks = rep->contents.delta.chunks;

  /* Initialize *KEYS to an empty array. */
  *keys = apr_array_make (pool, chunks->nelts, sizeof (key));
  if (! chunks->nelts)
    return SVN_NO_ERROR;
  
  /* Now, push the string keys for each window into *KEYS */
  for (i = 0; i < chunks->nelts; i++)
    {
      svn_fs__rep_delta_chunk_t *chunk = 
        (((svn_fs__rep_delta_chunk_t **) chunks->elts)[i]);
      
      key = apr_pstrdup (pool, chunk->string_key);
      (*((const char **)(apr_array_push (*keys)))) = key;
    }

  return SVN_NO_ERROR;
}


/* Delete the strings associated with array KEYS in FS as part of TRAIL.  */
static svn_error_t *
delete_strings (apr_array_header_t *keys, 
                svn_fs_t *fs, 
                trail_t *trail)
{
  int i;
  const char *str_key;

  for (i = 0; i < keys->nelts; i++)
    {
      str_key = ((const char **) keys->elts)[i];
      SVN_ERR (svn_fs__bdb_string_delete (fs, str_key, trail));
    }
  return SVN_NO_ERROR;
}



/*** Reading the contents from a representation. ***/

struct compose_handler_baton
{
  /* The combined window, and the pool it's allocated from. */
  svn_txdelta_window_t *window;
  apr_pool_t *window_pool;

  /* The trail for this operation. WINDOW_POOL will be a child of
     TRAIL->pool. No allocations will be made from TRAIL->pool itself. */
  trail_t *trail;

  /* TRUE when no more windows have to be read/combined. */
  svn_boolean_t done;

  /* TRUE if we've just started reading a new window. We need this
     because the svndiff handler will push a NULL window at the end of
     the stream, and we have to ignore that; but we must also know
     when it's appropriate to push a NULL window at the combiner. */
  svn_boolean_t init;
};


/* Handle one window. If BATON is emtpy, copy the WINDOW into it;
   otherwise, combine WINDOW with the one in BATON. */

static svn_error_t *
compose_handler (svn_txdelta_window_t *window, void *baton)
{
  struct compose_handler_baton *cb = baton;
  assert (!cb->done || window == NULL);
  assert (cb->trail && cb->trail->pool);

  if (!cb->init && !window)
    return SVN_NO_ERROR;

  if (cb->window)
    {
      /* Combine the incoming window with whatever's in the baton. */
      apr_pool_t *composite_pool = svn_pool_create (cb->trail->pool);
      svn_txdelta_window_t *composite;
      svn_txdelta__compose_ctx_t context = { 0 };

      composite = svn_txdelta__compose_windows
        (window, cb->window, &context, composite_pool);

      if (composite)
        {
          svn_pool_destroy (cb->window_pool);
          cb->window = composite;
          cb->window_pool = composite_pool;
        }
      else if (context.use_second)
        {
          svn_pool_destroy (composite_pool);
          cb->window->sview_offset = context.sview_offset;
          cb->window->sview_len = context.sview_len;

          /* This can only happen if the window doesn't touch
             source data; so ... */
          cb->done = TRUE;
        }
      else
        /* Can't happen, because cb->window can't be NULL. */
        abort ();
    }
  else if (window)
    {
      /* Copy the (first) window into the baton. */
      apr_pool_t *window_pool = svn_pool_create (cb->trail->pool);
      assert (cb->window_pool == NULL);
      cb->window = svn_txdelta__copy_window(window, window_pool);
      cb->window_pool = window_pool;
      cb->done = (window->sview_len == 0 || window->src_ops == 0);
    }
  else
    cb->done = TRUE;

  cb->init = FALSE;
  return SVN_NO_ERROR;
}



/* Read one delta window from REP[CUR_CHUNK] and push it at the
   composition handler. */

static svn_error_t *
get_one_window (struct compose_handler_baton *cb,
                svn_fs_t *fs,
                svn_fs__representation_t *rep,
                int cur_chunk)
{
  svn_stream_t *wstream;
  char diffdata[4096];   /* hunk of svndiff data */
  svn_filesize_t off;    /* offset into svndiff data */
  apr_size_t amt;        /* how much svndiff data to/was read */
  const char *str_key;

  apr_array_header_t *chunks = rep->contents.delta.chunks;
  svn_fs__rep_delta_chunk_t *this_chunk, *first_chunk;

  cb->init = TRUE;
  if (chunks->nelts <= cur_chunk)
    return compose_handler (NULL, cb);

  /* Set up a window handling stream for the svndiff data. */
  wstream = svn_txdelta_parse_svndiff (compose_handler, cb, TRUE,
                                       cb->trail->pool);

  /* First things first:  send the "SVN"{version} header through the
     stream.  ### For now, we will just use the version specified
     in the first chunk, and then verify that no chunks have a
     different version number than the one used.  In the future,
     we might simply convert chunks that use a different version
     of the diff format -- or, heck, a different format
     altogether -- to the format/version of the first chunk.  */
  first_chunk = APR_ARRAY_IDX (chunks, 0, svn_fs__rep_delta_chunk_t*);
  diffdata[0] = 'S';
  diffdata[1] = 'V';
  diffdata[2] = 'N';
  diffdata[3] = (char) (first_chunk->version);
  amt = 4;
  SVN_ERR (svn_stream_write (wstream, diffdata, &amt));
  /* FIXME: The stream write handler is borked; assert (amt == 4); */

  /* Get this string key which holds this window's data.
     ### todo: make sure this is an `svndiff' DIFF skel here. */
  this_chunk = APR_ARRAY_IDX (chunks, cur_chunk, svn_fs__rep_delta_chunk_t*);
  str_key = this_chunk->string_key;

  /* Run through the svndiff data, at least as far as necessary. */
  off = 0;
  do
    {
      amt = sizeof (diffdata);
      SVN_ERR (svn_fs__bdb_string_read (fs, str_key, diffdata,
                                        off, &amt, cb->trail));
      off += amt;
      SVN_ERR (svn_stream_write (wstream, diffdata, &amt));
    }
  while (amt != 0);
  SVN_ERR (svn_stream_close (wstream));

  assert (!cb->init);
  assert (cb->window != NULL);
  assert (cb->window_pool != NULL);
  return SVN_NO_ERROR;
}


/* Undeltify a range of data. DELTAS is the set of delta windows to
   combine, FULLTEXT is the source text, CUR_CHUNK is the index of the
   delta chunk we're starting from. OFFSET is the relative offset of
   the requested data within the chunk; BUF and LEN are what we're
   undeltifying to. */

static svn_error_t *
rep_undeltify_range (svn_fs_t *fs,
                     apr_array_header_t *deltas,
                     svn_fs__representation_t *fulltext,
                     int cur_chunk,
                     char *buf,
                     apr_size_t offset,
                     apr_size_t *len,
                     trail_t *trail)
{
  apr_size_t len_read = 0;

  do
    {
      struct compose_handler_baton cb = { 0 };
      char *source_buf, *target_buf;
      apr_size_t target_len;
      int cur_rep;

      cb.trail = trail;
      cb.done = FALSE;
      for (cur_rep = 0; !cb.done && cur_rep < deltas->nelts; ++cur_rep)
        {
          svn_fs__representation_t *const rep =
            APR_ARRAY_IDX (deltas, cur_rep, svn_fs__representation_t*);
          SVN_ERR (get_one_window (&cb, fs, rep, cur_chunk));
        }

      if (!cb.window)
          /* That's it, no more source data is available. */
          break;

      /* The source view length should not be 0 if there are source
         copy ops in the window. */
      assert (cb.window->sview_len > 0 || cb.window->src_ops == 0);

      /* cb.window is the combined delta window. Read the source text
         into a buffer. */
      if (fulltext && cb.window->sview_len > 0 && cb.window->src_ops > 0)
        {
          apr_size_t source_len = cb.window->sview_len;
          source_buf = apr_palloc (cb.window_pool, source_len);
          SVN_ERR (svn_fs__bdb_string_read
                   (fs, fulltext->contents.fulltext.string_key,
                    source_buf, cb.window->sview_offset, &source_len, trail));
          assert (source_len == cb.window->sview_len);
        }
      else
        {
          static char empty_buf[] = "";
          source_buf = empty_buf; /* Won't read anything from here. */
        }

      if (offset > 0)
        {
          target_len = *len - len_read + offset;
          target_buf = apr_palloc (cb.window_pool, target_len);
        }
      else
        {
          target_len = *len - len_read;
          target_buf = buf;
        }

      svn_txdelta__apply_instructions (cb.window, source_buf,
                                       target_buf, &target_len);
      if (offset > 0)
        {
          assert (target_len > offset);
          target_len -= offset;
          memcpy (buf, target_buf + offset, target_len);
          offset = 0; /* Read from the beginning of the next chunk. */
        }
      /* Don't need this window any more. */
      svn_pool_destroy (cb.window_pool);

      len_read += target_len;
      buf += target_len;
      ++cur_chunk;
    }
  while (len_read < *len);

  *len = len_read;
  return SVN_NO_ERROR;
}



/* Calculate the index of the chunk in REP that contains REP_OFFSET,
   and find the relative CHUNK_OFFSET within the chunk.
   Return -1 if offset is beyond the end of the represented data.
   ### The basic assumption is that all delta windows are the same size
   and aligned at the same offset, so this number is the same in all
   dependent deltas.  Oh, and the chunks in REP must be ordered. */

static int
get_chunk_offset (svn_fs__representation_t *rep,
                  svn_filesize_t rep_offset,
                  apr_size_t *chunk_offset)
{
  const apr_array_header_t *chunks = rep->contents.delta.chunks;
  int cur_chunk;
  assert (chunks->nelts);

  /* ### Yes, this is a linear search.  I'll change this to bisection
     the very second we notice it's slowing us down. */
  for (cur_chunk = 0; cur_chunk < chunks->nelts; ++cur_chunk)
  {
    const svn_fs__rep_delta_chunk_t *const this_chunk
      = APR_ARRAY_IDX (chunks, cur_chunk, svn_fs__rep_delta_chunk_t*);

    if ((this_chunk->offset + this_chunk->size) > rep_offset)
      {
        assert (this_chunk->offset <= rep_offset);
        assert (rep_offset - this_chunk->offset < SVN_MAX_OBJECT_SIZE);
        *chunk_offset = (apr_size_t) (rep_offset - this_chunk->offset);
        return cur_chunk;
      }
  }

  return -1;
}

/* Copy into BUF *LEN bytes starting at OFFSET from the string
   represented via REP_KEY in FS, as part of TRAIL.
   The number of bytes actually copied is stored in *LEN.  */
static svn_error_t *
rep_read_range (svn_fs_t *fs,
                const char *rep_key,
                svn_filesize_t offset,
                char *buf,
                apr_size_t *len,
                trail_t *trail)
{
  svn_fs__representation_t *rep;
  apr_size_t chunk_offset;

  /* Read in our REP. */
  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));
  if (rep->kind == svn_fs__rep_kind_fulltext)
    {
      SVN_ERR (svn_fs__bdb_string_read (fs, rep->contents.fulltext.string_key, 
                                        buf, offset, len, trail));
    }
  else if (rep->kind == svn_fs__rep_kind_delta)
    {
      const int cur_chunk = get_chunk_offset (rep, offset, &chunk_offset);
      if (cur_chunk < 0)
        *len = 0;
      else
        {
          /* Make a list of all the rep's we need to undeltify this range.
             We'll have to read them within this trail anyway, so we might
             as well do it once and up front. */
          apr_array_header_t *reps =  /* ### what constant here? */
            apr_array_make (trail->pool, 666, sizeof (rep));
          do
            {
              const svn_fs__rep_delta_chunk_t *const first_chunk
                = APR_ARRAY_IDX (rep->contents.delta.chunks,
                                 0, svn_fs__rep_delta_chunk_t*);
              const svn_fs__rep_delta_chunk_t *const chunk
                = APR_ARRAY_IDX (rep->contents.delta.chunks,
                                 cur_chunk, svn_fs__rep_delta_chunk_t*);

              /* Verify that this chunk is of the same version as the first. */
              if (first_chunk->version != chunk->version)
                return svn_error_createf
                  (SVN_ERR_FS_CORRUPT, NULL,
                   "diff version inconsistencies in representation `%s'",
                   rep_key);

              rep_key = chunk->rep_key;
              *(svn_fs__representation_t**) apr_array_push (reps) = rep;
              SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));
            }
          while (rep->kind == svn_fs__rep_kind_delta
                 && rep->contents.delta.chunks->nelts > cur_chunk);

          /* Right. We've either just read the fulltext rep, a rep that's
             too short, in which case we'll undeltify without source data.*/
          if (rep->kind != svn_fs__rep_kind_delta
              && rep->kind != svn_fs__rep_kind_fulltext)
            abort(); /* unknown kind */

          if (rep->kind == svn_fs__rep_kind_delta)
            rep = NULL;         /* Don't use source data */
          SVN_ERR (rep_undeltify_range (fs, reps, rep, cur_chunk,
                                        buf, chunk_offset, len, trail));
        }
    }
  else /* unknown kind */
    abort ();

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__get_mutable_rep (const char **new_rep_key,
                         const char *rep_key,
                         svn_fs_t *fs,
                         const char *txn_id,
                         trail_t *trail)
{
  svn_fs__representation_t *rep = NULL;
  const char *new_str = NULL;

  /* We were passed an existing REP_KEY, so examine it.  If it is
     mutable already, then just return REP_KEY as the mutable result
     key.  */
  if (rep_key && (rep_key[0] != '\0'))
    {
      SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));
      if (rep_is_mutable (rep, txn_id))
        {
          *new_rep_key = rep_key;
          return SVN_NO_ERROR;
        }
    }
  
  /* Either we weren't provided a base key to examine, or the base key
     we were provided was not mutable.  So, let's make a new
     representation and return its key to the caller. */
  SVN_ERR (svn_fs__bdb_string_append (fs, &new_str, 0, NULL, trail));
  rep = make_fulltext_rep (new_str, txn_id, svn_md5_empty_string_digest,
                           trail->pool);
  SVN_ERR (svn_fs__bdb_write_new_rep (new_rep_key, fs, rep, trail));

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__delete_rep_if_mutable (svn_fs_t *fs,
                               const char *rep_key,
                               const char *txn_id,
                               trail_t *trail)
{
  svn_fs__representation_t *rep;

  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));
  if (! rep_is_mutable (rep, txn_id))
    return SVN_NO_ERROR;

  if (rep->kind == svn_fs__rep_kind_fulltext)
    {
      SVN_ERR (svn_fs__bdb_string_delete (fs, rep->contents.fulltext.string_key,
                                          trail));
    }
  else if (rep->kind == svn_fs__rep_kind_delta)
    {
      apr_array_header_t *keys;
      SVN_ERR (delta_string_keys (&keys, rep, trail->pool));
      SVN_ERR (delete_strings (keys, fs, trail));
    }
  else /* unknown kind */
    abort ();

  SVN_ERR (svn_fs__bdb_delete_rep (fs, rep_key, trail));
  return SVN_NO_ERROR;
}



/*** Reading and writing data via representations. ***/

/** Reading. **/

struct rep_read_baton
{
  /* The FS from which we're reading. */
  svn_fs_t *fs;

  /* The representation skel whose contents we want to read.  If this
     is NULL, the rep has never had any contents, so all reads fetch 0
     bytes.

     Formerly, we cached the entire rep skel here, not just the key.
     That way we didn't have to fetch the rep from the db every time
     we want to read a little bit more of the file.  Unfortunately,
     this has a problem: if, say, a file's representation changes
     while we're reading (changes from fulltext to delta, for
     example), we'll never know it.  So for correctness, we now
     refetch the representation skel every time we want to read
     another chunk.  */
  const char *rep_key;
  
  /* How many bytes have been read already. */
  svn_filesize_t offset;

  /* If present, the read will be done as part of this trail, and the
     trail's pool will be used.  Otherwise, see `pool' below.  */
  trail_t *trail;

  /* MD5 checksum.  Initialized when the baton is created, updated as
     we read data, and finalized when the stream is closed. */ 
  struct apr_md5_ctx_t md5_context;

  /* The length of the rep's contents (as fulltext, that is,
     independent of how the rep actually stores the data.)  This is
     retrieved when the baton is created, and used to determine when
     we have read the last byte, at which point we compare checksums.

     Getting this at baton creation time makes interleaved reads and
     writes on the same rep in the same trail impossible.  But we're
     not doing that, and probably no one ever should.  And anyway if
     they do, they should see problems immediately. */
  svn_filesize_t size;

  /* Set to FALSE when the baton is created, TRUE when the md5_context
     is digestified. */
  svn_boolean_t checksum_finalized;

  /* Used for temporary allocations, iff `trail' (above) is null.  */
  apr_pool_t *pool;

};


static svn_error_t *
rep_read_get_baton (struct rep_read_baton **rb_p,
                    svn_fs_t *fs,
                    const char *rep_key,
                    svn_boolean_t use_trail_for_reads,
                    trail_t *trail,
                    apr_pool_t *pool)
{
  struct rep_read_baton *b;

  b = apr_pcalloc (pool, sizeof (*b));
  apr_md5_init (&(b->md5_context));

  if (rep_key)
    SVN_ERR (svn_fs__rep_contents_size (&(b->size), fs, rep_key, trail));
  else
    b->size = 0;

  b->checksum_finalized = FALSE;
  b->fs = fs;
  b->trail = use_trail_for_reads ? trail : NULL;
  b->pool = pool;
  b->rep_key = rep_key;
  b->offset = 0;

  *rb_p = b;

  return SVN_NO_ERROR;
}



/*** Retrieving data. ***/

svn_error_t *
svn_fs__rep_contents_size (svn_filesize_t *size_p,
                           svn_fs_t *fs,
                           const char *rep_key,
                           trail_t *trail)
{
  svn_fs__representation_t *rep;

  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));

  if (rep->kind == svn_fs__rep_kind_fulltext)
    {
      /* Get the size by asking Berkeley for the string's length. */
      SVN_ERR (svn_fs__bdb_string_size (size_p, fs, 
                                        rep->contents.fulltext.string_key, trail));
    }
  else if (rep->kind == svn_fs__rep_kind_delta)
    {
      /* Get the size by finding the last window pkg in the delta and
         adding its offset to its size.  This way, we won't even be
         messed up by overlapping windows, as long as the window pkgs
         are still ordered. */
      apr_array_header_t *chunks = rep->contents.delta.chunks;
      svn_fs__rep_delta_chunk_t *last_chunk;

      assert (chunks->nelts);

      last_chunk 
        = (((svn_fs__rep_delta_chunk_t **) chunks->elts)[chunks->nelts - 1]);
      *size_p = last_chunk->offset + last_chunk->size;
    }
  else /* unknown kind */
    abort ();

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__rep_contents_checksum (unsigned char digest[],
                               svn_fs_t *fs,
                               const char *rep_key,
                               trail_t *trail)
{
  svn_fs__representation_t *rep;

  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));
  memcpy (digest, rep->checksum, MD5_DIGESTSIZE);

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__rep_contents (svn_string_t *str,
                      svn_fs_t *fs,
                      const char *rep_key,
                      trail_t *trail)
{
  svn_filesize_t contents_size;
  apr_size_t len;
  char *data;

  SVN_ERR (svn_fs__rep_contents_size (&contents_size, fs, rep_key, trail));

  /* What if the contents are larger than we can handle? */
  if (contents_size > SVN_MAX_OBJECT_SIZE)
    return svn_error_createf
      (SVN_ERR_FS_GENERAL, NULL,
       "svn_fs__rep_contents: rep contents are too large "
       "(got %" SVN_FILESIZE_T_FMT ", limit is %" APR_SIZE_T_FMT ")",
       contents_size, SVN_MAX_OBJECT_SIZE);
  else
    str->len = (apr_size_t) contents_size;

  data = apr_palloc (trail->pool, str->len);
  str->data = data;
  len = str->len;
  SVN_ERR (rep_read_range (fs, rep_key, 0, data, &len, trail));

  /* Paranoia. */
  if (len != str->len)
    return svn_error_createf
      (SVN_ERR_FS_CORRUPT, NULL,
       "svn_fs__rep_contents: failure reading rep \"%s\"", rep_key);

  /* Just the standard paranoia. */
  {
    svn_fs__representation_t *rep;
    apr_md5_ctx_t md5_context;
    unsigned char checksum[MD5_DIGESTSIZE];
    
    apr_md5_init (&md5_context);
    apr_md5_update (&md5_context, str->data, str->len);
    apr_md5_final (checksum, &md5_context);

    SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));
    if (! svn_md5_digests_match (checksum, rep->checksum))
      return svn_error_createf
        (SVN_ERR_FS_CORRUPT, NULL,
         "svn_fs__rep_contents: checksum mismatch on rep \"%s\":\n"
         "   expected:  %s\n"
         "     actual:  %s\n", rep_key,
         svn_md5_digest_to_cstring (rep->checksum, trail->pool),
         svn_md5_digest_to_cstring (checksum, trail->pool));
  }

  return SVN_NO_ERROR;
}


struct read_rep_args
{
  struct rep_read_baton *rb;   /* The data source.             */
  char *buf;                   /* Where to put what we read.   */
  apr_size_t *len;             /* How much to read / was read. */
};


/* BATON is of type `read_rep_args':

   Read into BATON->rb->buf the *(BATON->len) bytes starting at
   BATON->rb->offset from the data represented at BATON->rb->rep_key
   in BATON->rb->fs, as part of TRAIL.

   Afterwards, *(BATON->len) is the number of bytes actually read, and
   BATON->rb->offset is incremented by that amount.
   
   If BATON->rb->rep_key is null, this is assumed to mean the file's
   contents have no representation, i.e., the file has no contents.
   In that case, if BATON->rb->offset > 0, return the error
   SVN_ERR_FS_FILE_CONTENTS_CHANGED, else just set *(BATON->len) to
   zero and return.  */
static svn_error_t *
txn_body_read_rep (void *baton, trail_t *trail)
{
  struct read_rep_args *args = baton;

  if (args->rb->rep_key)
    {
      SVN_ERR (rep_read_range (args->rb->fs,
                               args->rb->rep_key,
                               args->rb->offset,
                               args->buf,
                               args->len,
                               trail));

      args->rb->offset += *(args->len);

      /* We calculate the checksum just once, the moment we see the
       * last byte of data.  But we can't assume there was a short
       * read.  The caller may have known the length of the data and
       * requested exactly that amount, so there would never be a
       * short read.  (That's why the read baton has to know the
       * length of the data in advance.)
       *
       * On the other hand, some callers invoke the stream reader in a
       * loop whose termination condition is that the read returned
       * zero bytes of data -- which usually results in the read
       * function being called one more time *after* the call that got
       * a short read (indicating end-of-stream).
       *
       * The conditions below ensure that we compare checksums even
       * when there is no short read associated with the last byte of
       * data, while also ensuring that it's harmless to repeatedly
       * read 0 bytes from the stream.
       */
      if (! args->rb->checksum_finalized)
        {
          apr_md5_update (&(args->rb->md5_context), args->buf, *(args->len));

          if (args->rb->offset == args->rb->size)
            {
              svn_fs__representation_t *rep;
              unsigned char checksum[MD5_DIGESTSIZE];
              
              apr_md5_final (checksum, &(args->rb->md5_context));
              args->rb->checksum_finalized = TRUE;

              SVN_ERR (svn_fs__bdb_read_rep (&rep, args->rb->fs,
                                             args->rb->rep_key, trail));
              if (! svn_md5_digests_match (checksum, rep->checksum))
                return svn_error_createf
                  (SVN_ERR_FS_CORRUPT, NULL,
                   "txn_body_read_rep: checksum mismatch on rep \"%s\":\n"
                   "   expected:  %s\n"
                   "     actual:  %s\n", args->rb->rep_key,
                   svn_md5_digest_to_cstring (rep->checksum, trail->pool),
                   svn_md5_digest_to_cstring (checksum, trail->pool));
            }
        }
    }
  else if (args->rb->offset > 0)
    {
      return
        svn_error_create
        (SVN_ERR_FS_REP_CHANGED, NULL,
         "txn_body_read_rep: null rep, but offset past zero already");
    }
  else
    *(args->len) = 0;

  return SVN_NO_ERROR;
}


static svn_error_t *
rep_read_contents (void *baton, char *buf, apr_size_t *len)
{
  struct rep_read_baton *rb = baton;
  struct read_rep_args args;

  args.rb = rb;
  args.buf = buf;
  args.len = len;

  /* If we got a trail, use it; else make one. */
  if (rb->trail)
    SVN_ERR (txn_body_read_rep (&args, rb->trail));
  else
    {
      /* Hey, guess what?  trails don't clear their own subpools.  In
         the case of reading from the db, any returned data should
         live in our pre-allocated buffer, so the whole operation can
         happen within a single malloc/free cycle.  This prevents us
         from creating millions of unnecessary trail subpools when
         reading a big file. */
      apr_pool_t *subpool = svn_pool_create (rb->pool);
      SVN_ERR (svn_fs__retry_txn (rb->fs,
                                  txn_body_read_rep,
                                  &args,
                                  subpool));
      svn_pool_destroy (subpool);
    }
  return SVN_NO_ERROR;
}


/** Writing. **/


struct rep_write_baton
{
  /* The FS in which we're writing. */
  svn_fs_t *fs;

  /* The representation skel whose contents we want to write. */
  const char *rep_key;
  
  /* The transaction id under which this write action will take
     place. */
  const char *txn_id;

  /* If present, do the write as part of this trail, and use trail's
     pool.  Otherwise, see `pool' below.  */ 
  trail_t *trail;

  /* MD5 checksum.  Initialized when the baton is created, updated as
     we write data, and finalized and stored when the stream is
     closed. */
  struct apr_md5_ctx_t md5_context;
  unsigned char md5_digest[MD5_DIGESTSIZE];
  svn_boolean_t finalized;

  /* Used for temporary allocations, iff `trail' (above) is null.  */
  apr_pool_t *pool;

};


static struct rep_write_baton *
rep_write_get_baton (svn_fs_t *fs,
                     const char *rep_key,
                     const char *txn_id,
                     trail_t *trail,
                     apr_pool_t *pool)
{
  struct rep_write_baton *b;

  b = apr_pcalloc (pool, sizeof (*b));
  apr_md5_init (&(b->md5_context));
  b->fs = fs;
  b->trail = trail;
  b->pool = pool;
  b->rep_key = rep_key;
  b->txn_id = txn_id;
  return b;
}



/* Write LEN bytes from BUF into the end of the string represented via
   REP_KEY in FS, as part of TRAIL.  If the representation is not
   mutable, return the error SVN_FS_REP_NOT_MUTABLE. */
static svn_error_t *
rep_write (svn_fs_t *fs,
           const char *rep_key,
           const char *buf,
           apr_size_t len,
           const char *txn_id,
           trail_t *trail)
{
  svn_fs__representation_t *rep;
        
  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));

  if (! rep_is_mutable (rep, txn_id))
    svn_error_createf
      (SVN_ERR_FS_REP_NOT_MUTABLE, NULL,
       "rep_write: rep \"%s\" is not mutable", rep_key);

  if (rep->kind == svn_fs__rep_kind_fulltext)
    {
      SVN_ERR (svn_fs__bdb_string_append
               (fs, &(rep->contents.fulltext.string_key), len, buf, trail));
    }
  else if (rep->kind == svn_fs__rep_kind_delta)
    {
      /* There should never be a case when we have a mutable
         non-fulltext rep.  The only code that creates mutable reps is
         in this file, and it creates them fulltext. */
      return svn_error_createf
        (SVN_ERR_FS_CORRUPT, NULL,
         "rep_write: rep \"%s\" both mutable and non-fulltext", rep_key);
    }
  else /* unknown kind */
    abort ();

  return SVN_NO_ERROR;
}


struct write_rep_args
{
  struct rep_write_baton *wb;   /* Destination.       */
  const char *buf;              /* Data.              */
  apr_size_t len;               /* How much to write. */
};


/* BATON is of type `write_rep_args':
   Append onto BATON->wb->rep_key's contents BATON->len bytes of
   data from BATON->wb->buf, in BATON->rb->fs, as part of TRAIL.  

   If the representation is not mutable, return the error
   SVN_FS_REP_NOT_MUTABLE.  */
static svn_error_t *
txn_body_write_rep (void *baton, trail_t *trail)
{
  struct write_rep_args *args = baton;

  SVN_ERR (rep_write (args->wb->fs,
                      args->wb->rep_key,
                      args->buf,
                      args->len,
                      args->wb->txn_id,
                      trail));

  apr_md5_update (&(args->wb->md5_context), args->buf, args->len);

  return SVN_NO_ERROR;
}


static svn_error_t *
rep_write_contents (void *baton, 
                    const char *buf, 
                    apr_size_t *len)
{
  struct rep_write_baton *wb = baton;
  struct write_rep_args args;

  /* We toss LEN's indirectness because if not all the bytes are
     written, it's an error, so we wouldn't be reporting anything back
     through *LEN anyway. */
  args.wb = wb;
  args.buf = buf;
  args.len = *len;

  /* If we got a trail, use it; else make one. */
  if (wb->trail)
    SVN_ERR (txn_body_write_rep (&args, wb->trail));
  else
    {
      /* Hey, guess what?  trails don't clear their own subpools.  In
         the case of simply writing the rep to the db, we're *certain*
         that there's no data coming back to us that needs to be
         preserved... so the whole operation can happen within a
         single malloc/free cycle.  This prevents us from creating
         millions of unnecessary trail subpools when writing a big
         file. */
      apr_pool_t *subpool = svn_pool_create (wb->pool);
      SVN_ERR (svn_fs__retry_txn (wb->fs,
                                  txn_body_write_rep,
                                  &args,
                                  subpool));
      svn_pool_destroy (subpool);
    }

  return SVN_NO_ERROR;
}


/* Helper for rep_write_close_contents(); see that doc string for
   more.  BATON is of type `struct rep_write_baton'. */
static svn_error_t *
txn_body_write_close_rep (void *baton, trail_t *trail)
{
  struct rep_write_baton *wb = baton;
  svn_fs__representation_t *rep;

  SVN_ERR (svn_fs__bdb_read_rep (&rep, wb->fs, wb->rep_key, trail));
  memcpy (rep->checksum, wb->md5_digest, MD5_DIGESTSIZE);
  SVN_ERR (svn_fs__bdb_write_rep (wb->fs, wb->rep_key, rep, trail));

  return SVN_NO_ERROR;
}


/* BATON is of type `struct rep_write_baton'. 
 *
 * Finalize BATON->md5_context and store the resulting digest under
 * BATON->rep_key.
 */
static svn_error_t *
rep_write_close_contents (void *baton)
{
  struct rep_write_baton *wb = baton;

  /* ### Thought: if we fixed apr-util MD5 contexts to allow repeated
     digestification, then we wouldn't need a stream close function at
     all -- instead, we could update the stored checksum each time a
     write occurred, which would have the added advantage of making
     interleaving reads and writes work.  Currently, they'd fail with
     a checksum mismatch, it just happens that our code never tries to
     do that anyway. */

  if (! wb->finalized)
    {
      apr_md5_final (wb->md5_digest, &wb->md5_context);
      wb->finalized = TRUE;
    }

  /* If we got a trail, use it; else make one. */
  if (wb->trail)
    {
      SVN_ERR (txn_body_write_close_rep (wb, wb->trail));
    }
  else
    {
      SVN_ERR (svn_fs__retry_txn (wb->fs,
                                  txn_body_write_close_rep,
                                  wb,
                                  wb->pool));
    }

  return SVN_NO_ERROR;
}


/** Public read and write stream constructors. **/

svn_error_t *
svn_fs__rep_contents_read_stream (svn_stream_t **rs_p,
                                  svn_fs_t *fs,
                                  const char *rep_key,
                                  svn_boolean_t use_trail_for_reads,
                                  trail_t *trail,
                                  apr_pool_t *pool)
{
  struct rep_read_baton *rb;

  SVN_ERR (rep_read_get_baton (&rb, fs, rep_key, use_trail_for_reads,
                               trail, pool));
  *rs_p = svn_stream_create (rb, pool);
  svn_stream_set_read (*rs_p, rep_read_contents);

  return SVN_NO_ERROR;
}


/* Clear the contents of REP_KEY, so that it represents the empty
   string, as part of TRAIL.  TXN_ID is the id of the Subversion
   transaction under which this occurs.  If REP_KEY is not mutable,
   return the error SVN_ERR_FS_REP_NOT_MUTABLE.  */
static svn_error_t *
rep_contents_clear (svn_fs_t *fs,
                    const char *rep_key,
                    const char *txn_id,
                    trail_t *trail)
{
  svn_fs__representation_t *rep;
  const char *str_key;

  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));

  /* Make sure it's mutable. */
  if (! rep_is_mutable (rep, txn_id))
    return svn_error_createf
      (SVN_ERR_FS_REP_NOT_MUTABLE, NULL,
       "svn_fs__rep_contents_clear: rep \"%s\" is not mutable", rep_key);

  assert (rep->kind == svn_fs__rep_kind_fulltext);

  /* If rep has no string, just return success.  Else, clear the
     underlying string.  */
  str_key = rep->contents.fulltext.string_key;
  if (str_key && *str_key)
    {
      SVN_ERR (svn_fs__bdb_string_clear (fs, str_key, trail));
      memcpy (rep->checksum, svn_md5_empty_string_digest, MD5_DIGESTSIZE);
      SVN_ERR (svn_fs__bdb_write_rep (fs, rep_key, rep, trail));
    }
  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__rep_contents_write_stream (svn_stream_t **ws_p,
                                   svn_fs_t *fs,
                                   const char *rep_key,
                                   const char *txn_id,
                                   svn_boolean_t use_trail_for_writes,
                                   trail_t *trail,
                                   apr_pool_t *pool)
{
  struct rep_write_baton *wb;

  /* Clear the current rep contents (free mutability check!). */
  SVN_ERR (rep_contents_clear (fs, rep_key, txn_id, trail));

  /* Now, generate the write baton and stream. */
  wb = rep_write_get_baton (fs, rep_key, txn_id,
                            use_trail_for_writes ? trail : NULL, pool);
  *ws_p = svn_stream_create (wb, pool);
  svn_stream_set_write (*ws_p, rep_write_contents);
  svn_stream_set_close (*ws_p, rep_write_close_contents);

  return SVN_NO_ERROR;
}



/*** Deltified storage. ***/

/* Baton for svn_write_fn_t write_string(). */
struct write_string_baton
{
  /* The fs where lives the string we're writing. */
  svn_fs_t *fs;

  /* The key of the string we're writing to.  Typically this is
     initialized to NULL, so svn_fs__string_append() can fill in a
     value. */
  const char *key;

  /* The trail we're writing in. */
  trail_t *trail;
};


/* Function of type `svn_write_fn_t', for writing to a string;
   BATON is `struct write_string_baton *'.

   On the first call, BATON->key is null.  A new string key in
   BATON->fs is chosen and stored in BATON->key; each call appends
   *LEN bytes from DATA onto the string.  *LEN is never changed; if
   the write fails to write all *LEN bytes, an error is returned.  */
static svn_error_t *
write_string (void *baton, const char *data, apr_size_t *len)
{
  struct write_string_baton *wb = baton;
  return svn_fs__bdb_string_append (wb->fs, &(wb->key), *len, data, wb->trail);
}


/* Baton for svn_write_fn_t write_string_set(). */
struct write_svndiff_strings_baton
{
  /* The fs where lives the string we're writing. */
  svn_fs_t *fs;

  /* The key of the string we're writing to.  Typically this is
     initialized to NULL, so svn_fs__string_append() can fill in a
     value. */
  const char *key;

  /* The amount of txdelta data written to the current
     string-in-progress. */
  apr_size_t size;

  /* The amount of svndiff header information we've written thus far
     to the strings table. */
  apr_size_t header_read;

  /* The version number of the svndiff data written.  ### You'd better
     not count on this being populated after the first chunk is sent
     through the interface, since it lives at the 4th byte of the
     stream. */
  apr_byte_t version;

  /* The trail we're writing in. */
  trail_t *trail;

};


/* Function of type `svn_write_fn_t', for writing to a collection of
   strings; BATON is `struct write_svndiff_strings_baton *'.

   On the first call, BATON->key is null.  A new string key in
   BATON->fs is chosen and stored in BATON->key; each call appends
   *LEN bytes from DATA onto the string.  *LEN is never changed; if
   the write fails to write all *LEN bytes, an error is returned.
   BATON->size is used to track the total amount of data written via
   this handler, and must be reset by the caller to 0 when appropriate.  */
static svn_error_t *
write_svndiff_strings (void *baton, const char *data, apr_size_t *len)
{
  struct write_svndiff_strings_baton *wb = baton;
  const char *buf = data;
  apr_size_t nheader = 0;

  /* If we haven't stripped all the header information from this
     stream yet, keep stripping.  If someone sends a first window
     through here that's shorter than 4 bytes long, this will probably
     cause a nuclear reactor meltdown somewhere in the American
     midwest.  */
  if (wb->header_read < 4)
    {
      nheader = 4 - wb->header_read;
      *len -= nheader;
      buf += nheader;
      wb->header_read += nheader;
      
      /* If we have *now* read the full 4-byte header, check that
         least byte for the version number of the svndiff format. */
      if (wb->header_read == 4)
        wb->version = *(buf - 1);
    }
  
  /* Append to the current string we're writing (or create a new one
     if WB->key is NULL). */
  SVN_ERR (svn_fs__bdb_string_append (wb->fs, &(wb->key), *len, buf, wb->trail));

  /* Make sure we (still) have a key. */
  if (wb->key == NULL)
    return svn_error_create (SVN_ERR_FS_GENERAL, NULL,
                             "write_string_set: Failed to get new string key");

  /* Restore *LEN to the value it *would* have been were it not for
     header stripping. */
  *len += nheader;

  /* Increment our running total of bytes written to this string. */
  wb->size += *len;

  return SVN_NO_ERROR;
}


typedef struct window_write_t
{
  const char *key; /* string key for this window */
  apr_size_t svndiff_len; /* amount of svndiff data written to the string */
  svn_filesize_t text_off; /* offset of fulltext represented by this window */
  apr_size_t text_len; /* amount of fulltext data represented by this window */

} window_write_t;


svn_error_t *
svn_fs__rep_deltify (svn_fs_t *fs,
                     const char *target,
                     const char *source,
                     trail_t *trail)
{
  apr_pool_t *pool = trail->pool; /* convenience */
  svn_stream_t *source_stream; /* stream to read the source */
  svn_stream_t *target_stream; /* stream to read the target */
  svn_txdelta_stream_t *txdelta_stream; /* stream to read delta windows  */

  /* window-y things, and an array to track them */
  window_write_t *ww;
  apr_array_header_t *windows;

  /* stream to write new (deltified) target data and its baton */
  svn_stream_t *new_target_stream;
  struct write_svndiff_strings_baton new_target_baton;
  
  /* window handler/baton for writing to above stream */
  svn_txdelta_window_handler_t new_target_handler;
  void *new_target_handler_baton;
  
  /* yes, we do windows */
  svn_txdelta_window_t *window;

  /* The current offset into the fulltext that our window is about to
     write.  This doubles, after all windows are written, as the
     total size of the svndiff data for the deltification process. */
  svn_filesize_t tview_off = 0;

  /* The total amount of diff data written while deltifying. */
  svn_filesize_t diffsize = 0;

  /* TARGET's original string keys */
  apr_array_header_t *orig_str_keys;
  
  /* The digest for the representation's fulltext contents. */
  unsigned char rep_digest[MD5_DIGESTSIZE];

  /* MD5 digest */
  const unsigned char *digest;

  /* pool for holding the windows */
  apr_pool_t *wpool;

  /* Paranoia: never allow a rep to be deltified against itself,
     because then there would be no fulltext reachable in the delta
     chain, and badness would ensue.  */
  if (strcmp (target, source) == 0)
    return svn_error_createf
      (SVN_ERR_FS_CORRUPT, NULL,
       "svn_fs__rep_deltify: attempt to deltify \"%s\" against itself",
       target);

  /* Set up a handler for the svndiff data, which will write each
     window to its own string in the `strings' table. */
  new_target_baton.fs = fs;
  new_target_baton.trail = trail;
  new_target_baton.header_read = 0;
  new_target_stream = svn_stream_create (&new_target_baton, pool);
  svn_stream_set_write (new_target_stream, write_svndiff_strings);

  /* Get streams to our source and target text data. */
  SVN_ERR (svn_fs__rep_contents_read_stream (&source_stream, fs, source,
                                             TRUE, trail, pool));
  SVN_ERR (svn_fs__rep_contents_read_stream (&target_stream, fs, target,
                                             TRUE, trail, pool));

  /* Setup a stream to convert the textdelta data into svndiff windows. */
  svn_txdelta (&txdelta_stream, source_stream, target_stream, pool);
  svn_txdelta_to_svndiff (new_target_stream, pool,
                          &new_target_handler, &new_target_handler_baton);

  /* subpool for the windows */
  wpool = svn_pool_create (pool);

  /* Now, loop, manufacturing and dispatching windows of svndiff data. */
  windows = apr_array_make (pool, 1, sizeof (ww));
  do
    {
      /* Reset some baton variables. */
      new_target_baton.size = 0;
      new_target_baton.key = NULL;

      /* Fetch the next window of txdelta data. */
      SVN_ERR (svn_txdelta_next_window (&window, txdelta_stream, wpool));

      /* Send off this package to be written as svndiff data. */
      SVN_ERR (new_target_handler (window, new_target_handler_baton));
      if (window)
        {
          /* Add a new window description to our array. */
          ww = apr_pcalloc (pool, sizeof (*ww));
          ww->key = new_target_baton.key;
          ww->svndiff_len = new_target_baton.size;
          ww->text_off = tview_off;
          ww->text_len = window->tview_len;
          (*((window_write_t **)(apr_array_push (windows)))) = ww;

          /* Update our recordkeeping variables. */
          tview_off += window->tview_len;
          diffsize += ww->svndiff_len;
           
          /* Free the window. */
          svn_pool_clear (wpool);
        }

    } while (window);

  svn_pool_destroy (wpool);

  /* Having processed all the windows, we can query the MD5 digest
     from the stream.  */
  digest = svn_txdelta_md5_digest (txdelta_stream);
  if (! digest)
    return svn_error_createf
      (SVN_ERR_DELTA_MD5_CHECKSUM_ABSENT, NULL,
       "svn_fs__rep_deltify: failed to calculate MD5 digest for '%s'",
       source);

  /* Construct a list of the strings used by the old representation so
     that we can delete them later.  While we are here, if the old
     representation was a fulltext, check to make sure the delta we're
     replacing it with is actually smaller.  (Don't perform this check
     if we're replacing a delta; in that case, we're going for a time
     optimization, not a space optimization.)  */
  {
    svn_fs__representation_t *old_rep;
    const char *str_key;

    SVN_ERR (svn_fs__bdb_read_rep (&old_rep, fs, target, trail));
    if (old_rep->kind == svn_fs__rep_kind_fulltext)
      {
        svn_filesize_t old_size = 0;

        str_key = old_rep->contents.fulltext.string_key;
        SVN_ERR (svn_fs__bdb_string_size (&old_size, fs, str_key, trail));
        orig_str_keys = apr_array_make (pool, 1, sizeof (str_key));
        (*((const char **)(apr_array_push (orig_str_keys)))) = str_key;

        /* If the new data is NOT an space optimization, destroy the
           string(s) we created, and get outta here. */
        if (diffsize >= old_size)
          {
            int i;
            for (i = 0; i < windows->nelts; i++)
              {
                ww = ((window_write_t **) windows->elts)[i];
                SVN_ERR (svn_fs__bdb_string_delete (fs, ww->key, trail));
              }
            return SVN_NO_ERROR;
          }
      }
    else if (old_rep->kind == svn_fs__rep_kind_delta)
      SVN_ERR (delta_string_keys (&orig_str_keys, old_rep, pool));
    else /* unknown kind */
      abort ();

    /* Save the checksum, since the new rep needs it. */
    memcpy (rep_digest, old_rep->checksum, MD5_DIGESTSIZE);
  }

  /* Hook the new strings we wrote into the rest of the filesystem by
     building a new representation to replace our old one. */
  {
    svn_fs__representation_t new_rep;
    svn_fs__rep_delta_chunk_t *chunk;
    apr_array_header_t *chunks;
    int i;

    new_rep.kind = svn_fs__rep_kind_delta;
    new_rep.txn_id = NULL;

    /* Migrate the old rep's checksum to the new rep. */
    memcpy (new_rep.checksum, rep_digest, MD5_DIGESTSIZE);

    chunks = apr_array_make (pool, windows->nelts, sizeof (chunk));

    /* Loop through the windows we wrote, creating and adding new
       chunks to the representation. */
    for (i = 0; i < windows->nelts; i++)
      {
        ww = ((window_write_t **) windows->elts)[i];

        /* Allocate a chunk and its window */
        chunk = apr_palloc (pool, sizeof (*chunk));
        chunk->offset = ww->text_off;

        /* Populate the window */
        chunk->version = new_target_baton.version;
        chunk->string_key = ww->key;
        chunk->size = ww->text_len;
        memcpy (&(chunk->checksum), digest, MD5_DIGESTSIZE);
        chunk->rep_key = source;

        /* Add this chunk to the array. */
        (*((svn_fs__rep_delta_chunk_t **)(apr_array_push (chunks)))) = chunk;
      }
    
    /* Put the chunks array into the representation. */
    new_rep.contents.delta.chunks = chunks;

    /* Write out the new representation. */
    SVN_ERR (svn_fs__bdb_write_rep (fs, target, &new_rep, trail));

    /* Delete the original pre-deltified strings. */
    SVN_ERR (delete_strings (orig_str_keys, fs, trail));
  }

  return SVN_NO_ERROR;
}


svn_error_t *
svn_fs__rep_undeltify (svn_fs_t *fs,
                       const char *rep_key,
                       trail_t *trail)
{
  /* ### todo:  Make this thing `delta'-aware!   */
  /* (Uh, what does that mean?  -kfogel, 6 Jan 2003) */
  svn_fs__representation_t *rep;
  svn_stream_t *source_stream; /* stream to read the source */
  svn_stream_t *target_stream; /* stream to write the fulltext */
  struct write_string_baton target_baton;
  apr_array_header_t *orig_keys;
  apr_size_t len;
  apr_pool_t *subpool;
  char *buf;

  struct apr_md5_ctx_t context;
  unsigned char digest[MD5_DIGESTSIZE];

  /* Read the rep skel. */
  SVN_ERR (svn_fs__bdb_read_rep (&rep, fs, rep_key, trail));

  /* If REP is a fulltext rep, there's nothing to do. */
  if (rep->kind == svn_fs__rep_kind_fulltext)
    return SVN_NO_ERROR;
  if (rep->kind != svn_fs__rep_kind_delta)
    abort ();

  /* Get the original string keys from REP (so we can delete them after
     we write our new skel out. */
  SVN_ERR (delta_string_keys (&orig_keys, rep, trail->pool));

  /* Set up a string to receive the svndiff data. */
  target_baton.fs = fs;
  target_baton.trail = trail;
  target_baton.key = NULL;
  target_stream = svn_stream_create (&target_baton, trail->pool);
  svn_stream_set_write (target_stream, write_string);

  /* Set up the source stream. */
  SVN_ERR (svn_fs__rep_contents_read_stream (&source_stream, fs, rep_key,
                                             TRUE, trail, trail->pool));

  apr_md5_init (&context);
  subpool = svn_pool_create (trail->pool);
  buf = apr_palloc (subpool, SVN_STREAM_CHUNK_SIZE);
  do
    {
      apr_size_t len_read;

      len = SVN_STREAM_CHUNK_SIZE;
      SVN_ERR (svn_stream_read (source_stream, buf, &len));
      apr_md5_update (&context, buf, len);
      len_read = len;
      SVN_ERR (svn_stream_write (target_stream, buf, &len));
      if (len_read != len)
        return svn_error_createf
          (SVN_ERR_FS_GENERAL, NULL,
           "svn_fs__rep_undeltify: Error writing fulltext contents");
    }
  while (len);
  svn_pool_destroy (subpool);

  apr_md5_final (digest, &context);

  if (! svn_md5_digests_match (rep->checksum, digest))
    return svn_error_createf
      (SVN_ERR_FS_CORRUPT, NULL,
       "svn_fs__rep_undeltify: checksum mismatch on rep \"%s\":\n"
       "   expected:  %s\n"
       "     actual:  %s\n", rep_key,
       svn_md5_digest_to_cstring (rep->checksum, trail->pool),
       svn_md5_digest_to_cstring (digest, trail->pool));

  /* Now `target_baton.key' has the key of the new string.  We
     should hook it into the representation.  So we make a new rep,
     write it out... */
  /* ### todo: pass `digest' for the second null below, when finishing
     issue #649. */
  rep = make_fulltext_rep (target_baton.key, NULL, NULL, trail->pool);
  SVN_ERR (svn_fs__bdb_write_rep (fs, rep_key, rep, trail));

  /* ...then we delete our original strings. */
  SVN_ERR (delete_strings (orig_keys, fs, trail));

  return SVN_NO_ERROR;
}
