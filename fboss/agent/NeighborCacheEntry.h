/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/SwSwitch.h"
#include "fboss/agent/FbossError.h"
#include "fboss/agent/types.h"
#include "fboss/agent/state/NeighborEntry.h"

#include <chrono>
#include <folly/MacAddress.h>
#include <folly/IPAddress.h>
#include <folly/Random.h>

/**
 * This class implements much of the neighbor resolution and unreachable
 * neighbor detection logic. It is loosely modeled after the state machine in
 * NDP (RFC 4681), but will be used for both V4 and V6 entries. Each entry
 * corresponds to a single NeighborEntry node in the SwitchState and
 * contains the fields necessary to program that entry.
 *
 * It is implemented as a state machine with the following states:
 *
 * REACHABLE - neighbor entry is recently known to be valid.
 *
 * STALE - neighbor entry was known to be valid, but has exceeded its lifetime.
 *         If the entry is being used, we will transition the entry to PROBE.
 *
 * PROBE - The entry was once valid and became STALE. The entry is being used,
 *         so we are actively sending solicitations for this entry to confirm
 *         its validity. Note that the entry is still programmed in the
 *         SwitchState so should still be reachable.
 *
 * INCOMPLETE - This is an entry that does not have all fields needed to program
 *              a valid entry. It should correspond to a pending entry for now.
 *
 * EXPIRED - This entry has sent MAX_PROBES solicitations and has not become
 *           reachable. This entry should be flushed from the cache.
 *
 * DELAY - This is currently unused in this implementation, but RFC 4681 uses
 *         it as a buffer between REACHABLE and STALE states.
 *
 * UNINITIALIZED - Placeholder on startup.
 *
 * Once an entry is created, it is responsible for scheduling the timeout for
 * its next update. When that timeout expires, the state machine is run and the
 * next update is scheduled. If the entry ever transitions to the EXPIRED state,
 * we do not schedule another update and the cache will flush the entry.
 *
 * There is no locking in this class. Instead, the class relies on the
 * synchronization provided by NeighborCache, which should lock around all calls
 * into the cache with a single cache level lock. This class should take care
 * not to call into NeighborCache functions multiple times, as this will likely
 * cause a deadlock.
 */
namespace facebook { namespace fboss {


enum class NeighborEntryState : uint8_t {
  UNINITIALIZED,
  INCOMPLETE,
  DELAY,
  PROBE,
  STALE,
  REACHABLE,
  EXPIRED,
};

template <typename NTable> class NeighborCache;

template <typename NTable>
class NeighborCacheEntry : private folly::AsyncTimeout {
 public:
  typedef typename NTable::Entry::AddressType AddressType;
  typedef NeighborCache<NTable> Cache;
  typedef NeighborCacheEntry<NTable> Entry;
  typedef NeighborEntryFields<AddressType> EntryFields;
  NeighborCacheEntry(EntryFields fields,
                     folly::EventBase* evb,
                     Cache* cache,
                     NeighborEntryState state)
      : AsyncTimeout(evb),
        fields_(fields),
        cache_(cache),
        evb_(evb),
        probesLeft_(cache_->getMaxNeighborProbes()) {
    enter(state);
  }

  NeighborCacheEntry(AddressType ip,
                     folly::MacAddress mac,
                     PortID port,
                     InterfaceID intf,
                     folly::EventBase* evb,
                     Cache* cache,
                     NeighborEntryState state)
      : NeighborCacheEntry(EntryFields(ip, mac, port, intf),
                           evb, cache, state) {}

  NeighborCacheEntry(AddressType ip,
                     InterfaceID intf,
                     PendingEntry ignored,
                     folly::EventBase* evb,
                     Cache* cache)
      : NeighborCacheEntry(EntryFields(ip, intf, ignored),
                           evb, cache, NeighborEntryState::INCOMPLETE) {}

  ~NeighborCacheEntry() {}

  /*
   * Main entry point for handling the entries. Since entries may be
   * flushed by higher layers (thrift call etc.), we make sure we both
   * run the state machine and schedule the next update synchronously.
   */
  void process() {
    CHECK(evb_->isInEventBaseThread());
    if (isScheduled()) {
      // This function should never reschedule a timeout, it should
      // only create one if one does not already exist.  If a timeout
      // exists, it is because some event was received that restarted
      // the state machine before the old timeout could be processed.
      return;
    }

    runStateMachine();
    if (state_ != NeighborEntryState::EXPIRED) {
      scheduleNextUpdate();
    }
  }

  static void destroy(std::shared_ptr<Entry> entry, folly::EventBase* evb) {
    evb->runInEventBaseThread([entry]() {
        entry->cancelTimeout();
    });
  }

  folly::MacAddress getMac() const {
    return fields_.mac;
  }

  AddressType getIP() const {
    return fields_.ip;
  }

  PortID getPortID() const {
    return fields_.port;
  }

  InterfaceID getIntfID() const {
    return fields_.interfaceID;
  }

  bool isPending() const {
    return fields_.pending;
  }

  const EntryFields& getFields() {
    return fields_;
  }

  void updateFields(const EntryFields& fields) {
    fields_ = fields;
  }

  bool fieldsMatch(const EntryFields& fields) const {
    return getIP() == fields.ip &&
      getMac() == fields.mac &&
      getIntfID() == fields.interfaceID &&
      getPortID() == fields.port &&
      isPending() == fields.pending;
  }

  NeighborEntryState getState() const {
    return state_;
  }

  void updateState(NeighborEntryState state) {
    enter(state);
  }

  void setPending() {
    enter(NeighborEntryState::STALE);
  }

  bool isProbing() const {
    return state_ == NeighborEntryState::PROBE ||
      state_ == NeighborEntryState::INCOMPLETE;
  }

 private:
  /*
   * We tell the cache that this entry needs to be processed. The cache is
   * responsible for serializing this with other flush or rx events to prevent
   * races.
   */
  virtual void timeoutExpired() noexcept {
    cache_->processEntry(getIP());
  }

  /*
   * Schedules an update on the evb_. This is done synchronously so that we
   * can have a destructor guard around both running the state machine and
   * scheduling the next update in timeoutExpired.
   */
  void scheduleNextUpdate() {
    CHECK(evb_->inRunningEventBaseThread());
    switch (state_) {
      case NeighborEntryState::REACHABLE:
        scheduleTimeout(calculateLifetime());
        break;
      case NeighborEntryState::STALE:
        scheduleTimeout(std::chrono::seconds(cache_->getStaleEntryInterval()));
        break;
      case NeighborEntryState::PROBE:
      case NeighborEntryState::INCOMPLETE:
        scheduleTimeout(std::chrono::seconds(1));
        break;
      case NeighborEntryState::EXPIRED:
        // This entry is expired and is already flushed. Don't schedule a
        // new update
        break;
      case NeighborEntryState::DELAY:
      case NeighborEntryState::UNINITIALIZED:
        // DELAY is currently unused so we should never transition to DELAY
        // UNINITIALIZED is a placeholder during initialization
        throw FbossError("Invalid change of cache entry state.");
    }
  }

  /*
   * Calculates the lifetime of a REACHABLE entry. The lifetime is
   * calculated as a uniform distribution between 0.5 * timeout & 1.5 * timeout.
   * This is what RFC 4681 recommends.
   *
   * TODO(aeckert): we could actually store the timepoint it will go stale at
   * so we can report it in the cli
   */
  std::chrono::milliseconds calculateLifetime() const {
    auto base = cache_->getBaseTimeout().count() * 1000;
    auto lifetime =  folly::Random::rand32(base) + (base / 2);
    return std::chrono::milliseconds(lifetime);
  }

  bool hasProbesLeft() const {
    return probesLeft_ > 0;
  }

  /*
   * Entry function for the state machine. Should only be called when we
   * first create an entry. Handles a few special cases and makes sure
   * we don't create entries in unexpected states.
   */
  void enter(NeighborEntryState state) {
    state_ = state;
    switch (state) {
      case NeighborEntryState::INCOMPLETE:
        // We have already sent out a solictation for this so decrement
        // probesLeft_
        probesLeft_ = cache_->getMaxNeighborProbes() - 1;
        break;
      case NeighborEntryState::REACHABLE:
        probesLeft_ = cache_->getMaxNeighborProbes();
        break;
      case NeighborEntryState::STALE:
        // For STALE entries, we might as well run the state machine right away.
        probesLeft_ = cache_->getMaxNeighborProbes();
        runStateMachine();
        break;
      case NeighborEntryState::PROBE:
      case NeighborEntryState::DELAY:
      case NeighborEntryState::UNINITIALIZED:
      case NeighborEntryState::EXPIRED:
        // We should never enter in any of these states
        throw FbossError("Tried to create entry with invalid state");
    }
    evb_->runInEventBaseThread([this]() {
        scheduleNextUpdate();
    });
  }

  void probeIfProbesLeft() {
    DCHECK(isProbing());
    if (hasProbesLeft()) {
      cache_->probeFor(getIP());
      --probesLeft_;
    } else {
      state_ = NeighborEntryState::EXPIRED;
    }
  }

  void probeStaleEntryIfHit() {
    DCHECK(state_ == NeighborEntryState::STALE);
    if (cache_->isHit(getIP())) {
      state_ = NeighborEntryState::PROBE;
      probeIfProbesLeft();
    }
  }

  void runStateMachine() {
    switch (state_) {
      case NeighborEntryState::INCOMPLETE:
      case NeighborEntryState::PROBE:
        // For PROBE and INCOMPLETE entries, we keep sending out arp/ndp
        // requests unless we don't receive a response in MAX_PROBE tries.
        probeIfProbesLeft();
        break;
      case NeighborEntryState::STALE:
        // For STALE entries, we check the hardware hit bit. If the hit bit is
        // set, meaning someone's using the entry, we change the state to PROBE.
        probeStaleEntryIfHit();
        break;
      case NeighborEntryState::REACHABLE:
        // If we are processing a REACHABLE entry, it must have become stale,
        state_ = NeighborEntryState::STALE;
        probeStaleEntryIfHit();
        break;
      case NeighborEntryState::EXPIRED:
      case NeighborEntryState::DELAY:
      case NeighborEntryState::UNINITIALIZED:
        // DELAY is currently unused so we should never find a DELAY entry
        throw FbossError("Found NeighborCacheEntry with invalid state");
    }
  }

  std::string getStateName(NeighborEntryState state) {
    switch(state) {
      case NeighborEntryState::REACHABLE:
        return "REACHABLE";
      case NeighborEntryState::STALE:
        return "STALE";
      case NeighborEntryState::PROBE:
        return "PROBE";
      case NeighborEntryState::INCOMPLETE:
        return "INCOMPLETE";
      case NeighborEntryState::EXPIRED:
        return "EXPIRED";
      case NeighborEntryState::DELAY:
        return "DELAY";
      case NeighborEntryState::UNINITIALIZED:
        return "UNINITIALIZED";
      default:
        throw FbossError("Found invalid state!");
    }
  }

  std::string getStateName() {
    return getStateName(state_);
  }

  // Fields needed to program the SwitchState
  EntryFields fields_;

  // Additional state kept per cache entry.
  Cache* cache_;
  folly::EventBase* evb_;
  NeighborEntryState state_{NeighborEntryState::UNINITIALIZED};
  uint8_t probesLeft_{0};
};

}} // facebook::fboss
