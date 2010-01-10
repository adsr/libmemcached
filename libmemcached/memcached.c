/*
  Memcached library
*/
#include "common.h"

memcached_st *memcached_create(memcached_st *ptr)
{
  if (ptr == NULL)
  {
    ptr= (memcached_st *)calloc(1, sizeof(memcached_st));

    if (! ptr)
    {
      return NULL; /*  MEMCACHED_MEMORY_ALLOCATION_FAILURE */
    }

    ptr->options.is_allocated= true;
  }
  else
  {
    memset(ptr, 0, sizeof(memcached_st));
  }

  ptr->options.is_initialized= true;

  memcached_set_memory_allocators(ptr, NULL, NULL, NULL, NULL);

  if (! memcached_result_create(ptr, &ptr->result))
  {
    memcached_free(ptr);
    return NULL;
  }
  ptr->poll_timeout= MEMCACHED_DEFAULT_TIMEOUT;
  ptr->connect_timeout= MEMCACHED_DEFAULT_TIMEOUT;
  ptr->retry_timeout= 0;
  ptr->distribution= MEMCACHED_DISTRIBUTION_MODULA;

  /* TODO, Document why we picked these defaults */
  ptr->io_msg_watermark= 500;
  ptr->io_bytes_watermark= 65 * 1024;

  WATCHPOINT_ASSERT_INITIALIZED(&ptr->result);

  return ptr;
}

void server_list_free(memcached_st *ptr, memcached_server_st *servers)
{
  uint32_t x;

  if (servers == NULL)
    return;

  for (x= 0; x < memcached_servers_count(servers); x++)
    if (servers[x].address_info)
    {
      freeaddrinfo(servers[x].address_info);
      servers[x].address_info= NULL;
    }

  if (ptr)
  {
    ptr->call_free(ptr, servers);
  }
  else
    free(servers);
}

void memcached_servers_reset(memcached_st *ptr)
{
  server_list_free(ptr, ptr->hosts);

  ptr->hosts= NULL;
  ptr->number_of_hosts= 0;
  ptr->cursor_server= 0;
  ptr->last_disconnected_server= NULL;
  ptr->server_failure_limit= 0;
}

void memcached_free(memcached_st *ptr)
{
  /* If we have anything open, lets close it now */
  memcached_quit(ptr);
  server_list_free(ptr, ptr->hosts);
  memcached_result_free(&ptr->result);

  if (ptr->on_cleanup)
    ptr->on_cleanup(ptr);

  if (ptr->continuum)
    ptr->call_free(ptr, ptr->continuum);

  if (memcached_is_allocated(ptr))
  {
    ptr->call_free(ptr, ptr);
  }
  else
  {
    ptr->options.is_initialized= false;
  }
}

/*
  clone is the destination, while source is the structure to clone.
  If source is NULL the call is the same as if a memcached_create() was
  called.
*/
memcached_st *memcached_clone(memcached_st *clone, memcached_st *source)
{
  memcached_return_t rc= MEMCACHED_SUCCESS;
  memcached_st *new_clone;

  if (source == NULL)
    return memcached_create(clone);

  if (clone && memcached_is_allocated(clone))
  {
    return NULL;
  }

  new_clone= memcached_create(clone);

  if (new_clone == NULL)
    return NULL;

  new_clone->flags= source->flags;
  new_clone->send_size= source->send_size;
  new_clone->recv_size= source->recv_size;
  new_clone->poll_timeout= source->poll_timeout;
  new_clone->connect_timeout= source->connect_timeout;
  new_clone->retry_timeout= source->retry_timeout;
  new_clone->distribution= source->distribution;
  new_clone->hash= source->hash;
  new_clone->distribution_hash= source->distribution_hash;
  new_clone->user_data= source->user_data;

  new_clone->snd_timeout= source->snd_timeout;
  new_clone->rcv_timeout= source->rcv_timeout;

  new_clone->on_clone= source->on_clone;
  new_clone->on_cleanup= source->on_cleanup;
  new_clone->call_free= source->call_free;
  new_clone->call_malloc= source->call_malloc;
  new_clone->call_realloc= source->call_realloc;
  new_clone->call_calloc= source->call_calloc;
  new_clone->get_key_failure= source->get_key_failure;
  new_clone->delete_trigger= source->delete_trigger;
  new_clone->server_failure_limit= source->server_failure_limit;
  new_clone->io_msg_watermark= source->io_msg_watermark;
  new_clone->io_bytes_watermark= source->io_bytes_watermark;
  new_clone->io_key_prefetch= source->io_key_prefetch;
  new_clone->number_of_replicas= source->number_of_replicas;

  if (source->hosts)
    rc= memcached_server_push(new_clone, source->hosts);

  if (rc != MEMCACHED_SUCCESS)
  {
    memcached_free(new_clone);

    return NULL;
  }


  if (source->prefix_key[0] != 0)
  {
    strcpy(new_clone->prefix_key, source->prefix_key);
    new_clone->prefix_key_length= source->prefix_key_length;
  }

  rc= run_distribution(new_clone);

  if (rc != MEMCACHED_SUCCESS)
  {
    memcached_free(new_clone);

    return NULL;
  }

  if (source->on_clone)
    source->on_clone(source, new_clone);

  return new_clone;
}

void *memcached_get_user_data(memcached_st *ptr)
{
  return ptr->user_data;
}

void *memcached_set_user_data(memcached_st *ptr, void *data)
{
  void *ret= ptr->user_data;
  ptr->user_data= data;

  return ret;
}
