/*
 * infinitesource.{cc,hh} -- element generates configurable infinite stream
 * of packets
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2006 Regents of the University of California
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include "infinitesource.hh"
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/router.hh>
#include <click/standard/scheduleinfo.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
CLICK_DECLS

InfiniteSource::InfiniteSource()
  : _packet(0), _task(this)
{
}

InfiniteSource::~InfiniteSource()
{
}

void *
InfiniteSource::cast(const char *n)
{
    if (strcmp(n, class_name()) == 0)
	return static_cast<Element *>(this);
    else if (strcmp(n, Notifier::EMPTY_NOTIFIER) == 0)
	return static_cast<Notifier *>(this);
    else
	return 0;
}

int
InfiniteSource::configure(Vector<String> &conf, ErrorHandler *errh)
{
    ActiveNotifier::initialize(Notifier::EMPTY_NOTIFIER, router());
  String data = "Random bullshit in a packet, at least 64 bytes long. Well, now it is.";
  counter_t limit = -1;
  int burstsize = 1;
  int datasize = -1;
  bool active = true, stop = false, timestamp = true;

  if (cp_va_kparse(conf, this, errh,
		   "DATA", cpkP, cpString, &data,
#if HAVE_INT64_TYPES
		   "LIMIT", cpkP, cpInteger64, &limit,
#else
		   "LIMIT", cpkP, cpInteger, &limit,
#endif
		   "BURST", cpkP, cpInteger, &burstsize,
		   "ACTIVE", cpkP, cpBool, &active,
		   "TIMESTAMP", 0, cpBool, &timestamp,
		   "LENGTH", 0, cpInteger, &datasize,
		   "DATASIZE", 0, cpInteger, &datasize, // deprecated
		   "STOP", 0, cpBool, &stop,
		   cpEnd) < 0)
    return -1;
  if (burstsize < 1)
    return errh->error("burst size must be >= 1");

  _data = data;
  _datasize = datasize;
  _limit = limit;
  _burstsize = burstsize;
  _count = 0;
  _active = active;
  _stop = stop;
  _timestamp = timestamp;

  setup_packet();

  return 0;
}

int
InfiniteSource::initialize(ErrorHandler *errh)
{
  if (output_is_push(0)) {
    ScheduleInfo::initialize_task(this, &_task, errh);
    _nonfull_signal = Notifier::downstream_full_signal(this, 0, &_task);
  }
  return 0;
}

void
InfiniteSource::cleanup(CleanupStage)
{
  if (_packet)
    _packet->kill();
}

bool
InfiniteSource::run_task(Task *)
{
    if (!_active || !_nonfull_signal)
	return false;
    int n = _burstsize;
    if (_limit >= 0 && _count + n >= (ucounter_t) _limit)
	n = (_count > (ucounter_t) _limit ? 0 : _limit - _count);
    for (int i = 0; i < n; i++) {
	Packet *p = _packet->clone();
	if (_timestamp)
	    p->timestamp_anno().assign_now();
	output(0).push(p);
    }
    _count += n;
    if (n > 0)
	_task.fast_reschedule();
    else if (_stop && _limit >= 0 && _count >= (ucounter_t) _limit)
	router()->please_stop_driver();
    return n > 0;
}

Packet *
InfiniteSource::pull(int)
{
    if (!_active) {
    done:
	if (Notifier::active())
	    sleep();
	return 0;
    }
    if (_limit >= 0 && _count >= (ucounter_t) _limit) {
	if (_stop)
	    router()->please_stop_driver();
	goto done;
    }
    _count++;
    Packet *p = _packet->clone();
    if (_timestamp)
	p->timestamp_anno().assign_now();
    return p;
}

void
InfiniteSource::setup_packet()
{
    if (_packet)
	_packet->kill();

    if (_datasize < 0)
	_packet = Packet::make(_data.data(), _data.length());
    else if (_datasize <= _data.length())
	_packet = Packet::make(_data.data(), _datasize);
    else {
	// make up some data to fill extra space
	StringAccum sa;
	while (sa.length() < _datasize)
	    sa << _data;
	_packet = Packet::make(sa.data(), _datasize);
    }
}

int
InfiniteSource::change_param(const String &s, Element *e, void *vparam,
			     ErrorHandler *errh)
{
    InfiniteSource *is = (InfiniteSource *)e;
    switch ((intptr_t)vparam) {

    case h_data:		// data
	is->_data = s;
	is->setup_packet();
	break;

    case h_limit: {		// limit
	int limit;
	if (!cp_integer(s, &limit))
	    return errh->error("limit parameter must be integer");
	is->_limit = limit;
	break;
    }

    case h_burst: {		// burstsize
	int burstsize;
	if (!cp_integer(s, &burstsize) || burstsize < 1)
	    return errh->error("burstsize parameter must be integer >= 1");
	is->_burstsize = burstsize;
	break;
    }

    case h_active: {		// active
	bool active;
	if (!cp_bool(s, &active))
	    return errh->error("active parameter must be boolean");
	is->_active = active;
	break;
    }

    case h_reset: {		// reset
	is->_count = 0;
	break;
    }

    case h_length: {		// datasize
	int datasize;
	if (!cp_integer(s, &datasize))
	    return errh->error("length must be integer");
	is->_datasize = datasize;
	is->setup_packet();
	break;
    }
    }

    if (is->_active
	&& (is->_limit < 0 || is->_count < (ucounter_t) is->_limit)) {
	if (is->output_is_push(0) && !is->_task.scheduled())
	    is->_task.reschedule();
	if (is->output_is_pull(0) && !is->Notifier::active())
	    is->wake();
    }
    return 0;
}

void
InfiniteSource::add_handlers()
{
    add_data_handlers("data", Handler::OP_READ | Handler::RAW | Handler::CALM, &_data);
    add_write_handler("data", change_param, h_data, Handler::RAW);
    add_data_handlers("limit", Handler::OP_READ | Handler::CALM, &_limit);
    add_write_handler("limit", change_param, h_limit);
    add_data_handlers("burst", Handler::OP_READ | Handler::CALM, &_burstsize);
    add_write_handler("burst", change_param, h_burst);
    add_data_handlers("active", Handler::OP_READ | Handler::CHECKBOX, &_active);
    add_write_handler("active", change_param, h_active);
    add_data_handlers("count", Handler::OP_READ, &_count);
    add_write_handler("reset", change_param, h_reset, Handler::BUTTON);
    add_data_handlers("length", Handler::OP_READ | Handler::CALM, &_datasize);
    add_write_handler("length", change_param, h_length);
    // deprecated
    add_data_handlers("burstsize", Handler::OP_READ | Handler::CALM | Handler::DEPRECATED, &_burstsize);
    add_write_handler("burstsize", change_param, h_burst);
    add_data_handlers("datasize", Handler::OP_READ | Handler::CALM | Handler::DEPRECATED, &_datasize);
    add_write_handler("datasize", change_param, h_length);
    //add_read_handler("notifier", read_param, h_notifier);
    if (output_is_push(0))
	add_task_handlers(&_task, &_nonfull_signal);
}

CLICK_ENDDECLS
EXPORT_ELEMENT(InfiniteSource)
