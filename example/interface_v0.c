/* -*- Mode: C; tab-width: 2; c-basic-offset: 2; indent-tabs-mode: nil -*- */
/**
 * This file contains an implementation of the callback interface for level 0
 * in the protocol library. You might want to have your copy of the protocol
 * specification next to your coffee ;-)
 */
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libmemcached/protocol_handler.h>
#include <libmemcached/byteorder.h>
#include "storage.h"

static protocol_binary_response_status noop_command_handler(const void *cookie,
                                                            protocol_binary_request_header *header,
                                                            memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_no_extras response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= PROTOCOL_BINARY_CMD_NOOP,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque
    }
  };

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status quit_command_handler(const void *cookie,
                                                            protocol_binary_request_header *header,
                                                            memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_no_extras response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= PROTOCOL_BINARY_CMD_QUIT,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque
    }
  };

  if (header->request.opcode == PROTOCOL_BINARY_CMD_QUIT)
    response_handler(cookie, header, (void*)&response);

  /* I need a better way to signal to close the connection */
  return PROTOCOL_BINARY_RESPONSE_EIO;
}

static protocol_binary_response_status get_command_handler(const void *cookie,
                                                           protocol_binary_request_header *header,
                                                           memcached_binary_protocol_raw_response_handler response_handler)
{
  uint8_t opcode= header->request.opcode;
  union {
    protocol_binary_response_get response;
    char buffer[4096];
  } msg= {
    .response.message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= opcode,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque
    }
  };

  struct item *item= get_item(header + 1, ntohs(header->request.keylen));
  if (item)
  {
    msg.response.message.body.flags= htonl(item->flags);
    char *ptr= (char*)(msg.response.bytes + sizeof(*header) + 4);
    uint32_t bodysize= 4;
    msg.response.message.header.response.cas= htonll(item->cas);
    if (opcode == PROTOCOL_BINARY_CMD_GETK || opcode == PROTOCOL_BINARY_CMD_GETKQ)
    {
      memcpy(ptr, item->key, item->nkey);
      msg.response.message.header.response.keylen= htons((uint16_t)item->nkey);
      ptr += item->nkey;
      bodysize += (uint32_t)item->nkey;
    }
    memcpy(ptr, item->data, item->size);
    bodysize += (uint32_t)item->size;
    msg.response.message.header.response.bodylen= htonl(bodysize);
    msg.response.message.header.response.extlen= 4;

    return response_handler(cookie, header, (void*)&msg);
  }
  else if (opcode == PROTOCOL_BINARY_CMD_GET || opcode == PROTOCOL_BINARY_CMD_GETK)
  {
    msg.response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
    return response_handler(cookie, header, (void*)&msg);
  }

  /* Q shouldn't report a miss ;-) */
  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status delete_command_handler(const void *cookie,
                                                              protocol_binary_request_header *header,
                                                              memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  char *key= ((char*)header) + sizeof(*header);
  protocol_binary_response_no_extras response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= header->request.opcode,
      .opaque= header->request.opaque
    }
  };

  if (!delete_item(key, keylen))
  {
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
    return response_handler(cookie, header, (void*)&response);
  }
  else if (header->request.opcode == PROTOCOL_BINARY_CMD_DELETE)
  {
    /* DELETEQ doesn't want success response */
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS);
    return response_handler(cookie, header, (void*)&response);
  }

  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status flush_command_handler(const void *cookie,
                                                             protocol_binary_request_header *header,
                                                             memcached_binary_protocol_raw_response_handler response_handler)
{
  uint8_t opcode= header->request.opcode;

  /* @fixme sett inn when! */
  flush(0);

  if (opcode == PROTOCOL_BINARY_CMD_FLUSH)
  {
    protocol_binary_response_no_extras response= {
      .message.header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    };
    return response_handler(cookie, header, (void*)&response);
  }

  return PROTOCOL_BINARY_RESPONSE_SUCCESS;
}

static protocol_binary_response_status arithmetic_command_handler(const void *cookie,
                                                                  protocol_binary_request_header *header,
                                                                  memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_request_incr *req= (void*)header;
  protocol_binary_response_incr response= {
    .message.header.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= header->request.opcode,
      .opaque= header->request.opaque,
    },
  };

  uint16_t keylen= ntohs(header->request.keylen);
  uint64_t initial= ntohll(req->message.body.initial);
  uint64_t delta= ntohll(req->message.body.delta);
  uint32_t expiration= ntohl(req->message.body.expiration);
  void *key= req->bytes + sizeof(req->bytes);
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;

  struct item *item= get_item(key, keylen);
  if (item == NULL)
  {
    item= create_item(key, keylen, NULL, sizeof(initial), 0, (time_t)expiration);
    if (item == NULL)
    {
      rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
    }
    else
    {
      memcpy(item->data, &initial, sizeof(initial));
      put_item(item);
    }
  }
  else
  {
    if (header->request.opcode == PROTOCOL_BINARY_CMD_INCREMENT ||
        header->request.opcode == PROTOCOL_BINARY_CMD_INCREMENTQ)
    {
      (*(uint64_t*)item->data) += delta;
    }
    else
    {
      if (delta > *(uint64_t*)item->data)
      {
        *(uint64_t*)item->data= 0;
      }
      else
      {
        *(uint64_t*)item->data -= delta;
      }
    }

    struct item *nitem= create_item(key, keylen, NULL, sizeof(initial), 0, item->exp);
    if (item == NULL)
    {
      rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
      delete_item(key, keylen);
    }
    else
    {
      memcpy(nitem->data, item->data, item->size);
      delete_item(key, keylen);
      put_item(nitem);
      item = nitem;
    }
  }

  response.message.header.response.status= htons(rval);
  if (rval == PROTOCOL_BINARY_RESPONSE_SUCCESS)
  {
    response.message.header.response.bodylen= ntohl(8);
    response.message.body.value= ntohll((*(uint64_t*)item->data));
    response.message.header.response.cas= ntohll(item->cas);

    if (header->request.opcode == PROTOCOL_BINARY_CMD_INCREMENTQ ||
        header->request.opcode == PROTOCOL_BINARY_CMD_DECREMENTQ)
    {
      return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status version_command_handler(const void *cookie,
                                                               protocol_binary_request_header *header,
                                                               memcached_binary_protocol_raw_response_handler response_handler)
{
  const char *versionstring= "1.0.0";
  union {
    protocol_binary_response_header packet;
    char buffer[256];
  } response= {
    .packet.response= {
      .magic= PROTOCOL_BINARY_RES,
      .opcode= PROTOCOL_BINARY_CMD_VERSION,
      .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
      .opaque= header->request.opaque,
      .bodylen= htonl((uint32_t)strlen(versionstring))
    }
  };

  memcpy(response.buffer + sizeof(response.packet), versionstring, strlen(versionstring));

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status concat_command_handler(const void *cookie,
                                                              protocol_binary_request_header *header,
                                                              memcached_binary_protocol_raw_response_handler response_handler)
{
  protocol_binary_response_status rval= PROTOCOL_BINARY_RESPONSE_SUCCESS;
  uint16_t keylen= ntohs(header->request.keylen);
  uint64_t cas= ntohll(header->request.cas);
  void *key= header + 1;
  uint32_t vallen= ntohl(header->request.bodylen) - keylen;
  void *val= (char*)key + keylen;

  struct item *item= get_item(key, keylen);
  struct item *nitem;
  if (item == NULL)
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_ENOENT;
  }
  else if (cas != 0 && cas != item->cas)
  {
    rval= PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
  }
  else if ((nitem= create_item(key, keylen, NULL, item->size + vallen,
                               item->flags, item->exp)) == NULL)
  {
    rval= PROTOCOL_BINARY_RESPONSE_ENOMEM;
  }
  else
  {
    if (header->request.opcode == PROTOCOL_BINARY_CMD_APPEND ||
        header->request.opcode == PROTOCOL_BINARY_CMD_APPENDQ)
    {
      memcpy(nitem->data, item->data, item->size);
      memcpy(((char*)(nitem->data)) + item->size, val, vallen);
    }
    else
    {
      memcpy(nitem->data, val, vallen);
      memcpy(((char*)(nitem->data)) + vallen, item->data, item->size);
    }
    delete_item(key, keylen);
    put_item(nitem);
    if (header->request.opcode == PROTOCOL_BINARY_CMD_APPEND ||
        header->request.opcode == PROTOCOL_BINARY_CMD_PREPEND)
    {
      protocol_binary_response_no_extras response= {
        .message= {
          .header.response= {
            .magic= PROTOCOL_BINARY_RES,
            .opcode= header->request.opcode,
            .status= htons(rval),
            .opaque= header->request.opaque,
            .cas= htonll(nitem->cas),
          }
        }
      };
      return response_handler(cookie, header, (void*)&response);
    }
  }

  return rval;
}

static protocol_binary_response_status set_command_handler(const void *cookie,
                                                           protocol_binary_request_header *header,
                                                           memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  size_t datalen= ntohl(header->request.bodylen) - keylen - 8;
  protocol_binary_request_replace *request= (void*)header;
  uint32_t flags= ntohl(request->message.body.flags);
  time_t timeout= (time_t)ntohl(request->message.body.expiration);
  char *key= ((char*)header) + sizeof(*header) + 8;
  char *data= key + keylen;

  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  if (header->request.cas != 0)
  {
    /* validate cas */
    struct item* item= get_item(key, keylen);
    if (item != NULL && item->cas != ntohll(header->request.cas))
    {
      response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
      return response_handler(cookie, header, (void*)&response);
    }
  }

  delete_item(key, keylen);
  struct item* item= create_item(key, keylen, data, datalen, flags, timeout);
  if (item == 0)
  {
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
  }
  else
  {
    put_item(item);
    /* SETQ shouldn't return a message */
    if (header->request.opcode == PROTOCOL_BINARY_CMD_SET)
    {
      response.message.header.response.cas= htonll(item->cas);
      return response_handler(cookie, header, (void*)&response);
    }
    return PROTOCOL_BINARY_RESPONSE_SUCCESS;
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status add_command_handler(const void *cookie,
                                                           protocol_binary_request_header *header,
                                                           memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  size_t datalen= ntohl(header->request.bodylen) - keylen - 8;
  protocol_binary_request_add *request= (void*)header;
  uint32_t flags= ntohl(request->message.body.flags);
  time_t timeout= (time_t)ntohl(request->message.body.expiration);
  char *key= ((char*)header) + sizeof(*header) + 8;
  char *data= key + keylen;

  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  struct item* item= get_item(key, keylen);
  if (item == NULL)
  {
    item= create_item(key, keylen, data, datalen, flags, timeout);
    if (item == 0)
      response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
    else
    {
      put_item(item);
      /* ADDQ shouldn't return a message */
      if (header->request.opcode == PROTOCOL_BINARY_CMD_ADD)
      {
        response.message.header.response.cas= htonll(item->cas);
        return response_handler(cookie, header, (void*)&response);
      }
      return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }
  }
  else
  {
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);
  }

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status replace_command_handler(const void *cookie,
                                                               protocol_binary_request_header *header,
                                                               memcached_binary_protocol_raw_response_handler response_handler)
{
  size_t keylen= ntohs(header->request.keylen);
  size_t datalen= ntohl(header->request.bodylen) - keylen - 8;
  protocol_binary_request_replace *request= (void*)header;
  uint32_t flags= ntohl(request->message.body.flags);
  time_t timeout= (time_t)ntohl(request->message.body.expiration);
  char *key= ((char*)header) + sizeof(*header) + 8;
  char *data= key + keylen;

  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= header->request.opcode,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  struct item* item= get_item(key, keylen);
  if (item == NULL)
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_ENOENT);
  else if (header->request.cas == 0 || ntohll(header->request.cas) == item->cas)
  {
    delete_item(key, keylen);
    item= create_item(key, keylen, data, datalen, flags, timeout);
    if (item == 0)
      response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_ENOMEM);
    else
    {
      put_item(item);
      /* REPLACEQ shouldn't return a message */
      if (header->request.opcode == PROTOCOL_BINARY_CMD_REPLACE)
      {
        response.message.header.response.cas= htonll(item->cas);
        return response_handler(cookie, header, (void*)&response);
      }
      return PROTOCOL_BINARY_RESPONSE_SUCCESS;
    }
  }
  else
    response.message.header.response.status= htons(PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS);

  return response_handler(cookie, header, (void*)&response);
}

static protocol_binary_response_status stat_command_handler(const void *cookie,
                                                            protocol_binary_request_header *header,
                                                            memcached_binary_protocol_raw_response_handler response_handler)
{
  /* Just send the terminating packet*/
  protocol_binary_response_no_extras response= {
    .message= {
      .header.response= {
        .magic= PROTOCOL_BINARY_RES,
        .opcode= PROTOCOL_BINARY_CMD_STAT,
        .status= htons(PROTOCOL_BINARY_RESPONSE_SUCCESS),
        .opaque= header->request.opaque
      }
    }
  };

  return response_handler(cookie, header, (void*)&response);
}

struct memcached_binary_protocol_callback_st interface_v0_impl= {
  .interface_version= 0,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_GET]= get_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_SET]= set_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_ADD]= add_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_REPLACE]= replace_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_DELETE]= delete_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_INCREMENT]= arithmetic_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_DECREMENT]= arithmetic_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_QUIT]= quit_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_FLUSH]= flush_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_GETQ]= get_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_NOOP]= noop_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_VERSION]= version_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_GETK]= get_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_GETKQ]= get_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_APPEND]= concat_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_PREPEND]= concat_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_STAT]= stat_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_SETQ]= set_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_ADDQ]= add_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_REPLACEQ]= replace_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_DELETEQ]= delete_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_INCREMENTQ]= arithmetic_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_DECREMENTQ]= arithmetic_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_QUITQ]= quit_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_FLUSHQ]= flush_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_APPENDQ]= concat_command_handler,
  .interface.v0.comcode[PROTOCOL_BINARY_CMD_PREPENDQ]= concat_command_handler,
};
