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

#include <boost/container/flat_map.hpp>
#include <list>
#include <mutex>
#include <string>
#include <utility>
#include "fboss/agent/ArpCache.h"
#include "fboss/agent/NdpCache.h"
#include "fboss/agent/NeighborUpdaterImpl.h"
#include "fboss/agent/StateObserver.h"
#include "fboss/agent/state/PortDescriptor.h"
#include "fboss/agent/types.h"

namespace facebook {
namespace fboss {

class SwitchState;
class StateDelta;

/**
 * This class handles all updates to neighbor entries. Whenever we perform an
 * action or receive a packet that should update the Arp or Ndp tables in the
 * SwitchState, it goes through this class first. This class stores Arp and NDP
 * caches for each vlan and those are the source of truth for neighbor entries.
 *
 * Those caches are self-managing and manage all changes to the Neighbor Tables
 * due to expiration or failed resolution.
 *
 * This class implements the StateObserver API and listens for all vlan added or
 * deleted events. It ignores all changes it receives related to arp/ndp tables,
 * as all those changes should originate from the caches stored in this class.
 */
class NeighborUpdater : public AutoRegisterStateObserver {
 public:
  explicit NeighborUpdater(SwSwitch* sw);
  ~NeighborUpdater() override;

  void stateUpdated(const StateDelta& delta) override;

  using NeighborCaches = NeighborUpdaterImpl::NeighborCaches;

  // Zero-cost forwarders
#define ARG_TEMPLATE_PARAMETER(TYPE, NAME) typename T_##NAME
#define ARG_RVALUE_REF_TYPE(TYPE, NAME) T_##NAME&& NAME
#define ARG_FORWARDER(TYPE, NAME) std::forward<T_##NAME>(NAME)
#define NEIGHBOR_UPDATER_METHOD(VISIBILITY, NAME, RETURN_TYPE, ...) \
  VISIBILITY:                                                       \
  template <ARG_LIST(ARG_TEMPLATE_PARAMETER, ##__VA_ARGS__)>        \
  RETURN_TYPE NAME(ARG_LIST(ARG_RVALUE_REF_TYPE, ##__VA_ARGS__)) {  \
    return impl_->NAME(ARG_LIST(ARG_FORWARDER, ##__VA_ARGS__));     \
  }
#define NEIGHBOR_UPDATER_METHOD_NO_ARGS(VISIBILITY, NAME, RETURN_TYPE) \
  VISIBILITY:                                                          \
  RETURN_TYPE NAME() {                                                 \
    return impl_->NAME();                                              \
  }
#include "fboss/agent/NeighborUpdater.def"
#undef NEIGHBOR_UPDATER_METHOD

 private:
  std::shared_ptr<NeighborCaches> createCaches(
      const SwitchState* state,
      const Vlan* vlan);
  void portChanged(
      const std::shared_ptr<Port>& oldPort,
      const std::shared_ptr<Port>& newPort);
  void aggregatePortChanged(
      const std::shared_ptr<AggregatePort>& oldAggPort,
      const std::shared_ptr<AggregatePort>& newAggPort);
  void sendNeighborUpdates(const VlanDelta& delta);

  // Forbidden copy constructor and assignment operator
  NeighborUpdater(NeighborUpdater const&) = delete;
  NeighborUpdater& operator=(NeighborUpdater const&) = delete;

  std::shared_ptr<NeighborUpdaterImpl> impl_;
  SwSwitch* sw_{nullptr};
};

} // namespace fboss
} // namespace facebook
