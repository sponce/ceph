// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <iostream>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "include/types.h"
#include "include/utime.h"
#include "objclass/objclass.h"

#include "cls_log_types.h"
#include "cls_log_ops.h"

#include "global/global_context.h"

CLS_VER(1,0)
CLS_NAME(log)

cls_handle_t h_class;
cls_method_handle_t h_log_add;
cls_method_handle_t h_log_list;
cls_method_handle_t h_log_trim;

static string log_index_prefix = "1_";


static int write_log_entry(cls_method_context_t hctx, string& index, cls_log_entry& entry)
{
  bufferlist bl;
  ::encode(entry, bl);

  int ret = cls_cxx_map_set_val(hctx, index, &bl);
  if (ret < 0)
    return ret;

  return 0;
}

static void get_index_time_prefix(utime_t& ts, string& index)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%010ld.%06ld_", (long)ts.sec(), (long)ts.usec());

  index = log_index_prefix + buf;
}

static void get_index(cls_method_context_t hctx, utime_t& ts, string& index)
{
  get_index_time_prefix(ts, index);

  string unique_id;

  cls_cxx_subop_version(hctx, &unique_id);

  index.append(unique_id);
}

static int cls_log_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_add_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(1, "ERROR: cls_log_add_op(): failed to decode op");
    return -EINVAL;
  }

  cls_log_entry& entry = op.entry;

  string index;

  get_index(hctx, entry.timestamp, index);

  CLS_LOG(0, "storing entry at %s", index.c_str());

  int ret = write_log_entry(hctx, index, entry);
  if (ret < 0)
    return ret;
  
  return 0;
}

static int cls_log_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_list_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(1, "ERROR: cls_log_list_op(): failed to decode op");
    return -EINVAL;
  }

  map<string, bufferlist> keys;

  string from_index;
  string to_index;

  if (op.marker.empty()) {
    get_index_time_prefix(op.from_time, from_index);
  } else {
    from_index = op.marker;
  }
  bool use_time_boundary = (op.to_time > op.from_time);

  if (use_time_boundary)
    get_index_time_prefix(op.to_time, to_index);

#define MAX_ENTRIES 1000
  size_t max_entries = op.max_entries;
  if (!max_entries || max_entries > MAX_ENTRIES)
    max_entries = MAX_ENTRIES;

  int rc = cls_cxx_map_get_vals(hctx, from_index, log_index_prefix, max_entries + 1, &keys);
  if (rc < 0)
    return rc;

  cls_log_list_ret ret;

  list<cls_log_entry>& entries = ret.entries;
  map<string, bufferlist>::iterator iter = keys.begin();

  bool done = false;
  string marker;

  size_t i;
  for (i = 0; i < max_entries && iter != keys.end(); ++i, ++iter) {
    const string& index = iter->first;
    marker = index;
    if (use_time_boundary && index.compare(0, to_index.size(), to_index) >= 0) {
      done = true;
      break;
    }

    bufferlist& bl = iter->second;
    bufferlist::iterator biter = bl.begin();
    try {
      cls_log_entry e;
      ::decode(e, biter);
      entries.push_back(e);
    } catch (buffer::error& err) {
      CLS_LOG(0, "ERROR: cls_log_list: could not decode entry, index=%s", index.c_str());
    }
  }

  if (iter == keys.end())
    done = true;
  else
    ret.marker = marker;

  ret.truncated = !done;

  ::encode(ret, *out);

  return 0;
}


static int cls_log_trim(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_trim_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(0, "ERROR: cls_log_list_op(): failed to decode entry");
    return -EINVAL;
  }

  map<string, bufferlist> keys;

  string from_index;
  string to_index;

  get_index_time_prefix(op.from_time, from_index);
  get_index_time_prefix(op.to_time, to_index);

#define MAX_TRIM_ENTRIES 1000
  size_t max_entries = MAX_TRIM_ENTRIES;

  int rc = cls_cxx_map_get_vals(hctx, from_index, log_index_prefix, max_entries, &keys);
  if (rc < 0)
    return rc;

  map<string, bufferlist>::iterator iter = keys.begin();

  size_t i;
  bool removed = false;
  for (i = 0; i < max_entries && iter != keys.end(); ++i, ++iter) {
    const string& index = iter->first;

    CLS_LOG(20, "index=%s to_index=%s", index.c_str(), to_index.c_str());

    if (index.compare(0, to_index.size(), to_index) >= 0)
      break;

    CLS_LOG(20, "removing key: index=%s", index.c_str());

    int rc = cls_cxx_map_remove_key(hctx, index);
    if (rc < 0) {
      CLS_LOG(1, "ERROR: cls_cxx_map_remove_key failed rc=%d", rc);
      return -EINVAL;
    }
    removed = true;
  }

  if (!removed)
    return -ENODATA;

  return 0;
}

void __cls_init()
{
  CLS_LOG(1, "Loaded log class!");

  cls_register("log", &h_class);

  /* log */
  cls_register_cxx_method(h_class, "add", CLS_METHOD_RD | CLS_METHOD_WR, cls_log_add, &h_log_add);
  cls_register_cxx_method(h_class, "list", CLS_METHOD_RD, cls_log_list, &h_log_list);
  cls_register_cxx_method(h_class, "trim", CLS_METHOD_RD | CLS_METHOD_WR, cls_log_trim, &h_log_trim);

  return;
}

