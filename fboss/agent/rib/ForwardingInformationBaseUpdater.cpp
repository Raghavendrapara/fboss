/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/rib/ForwardingInformationBaseUpdater.h"

#include "fboss/agent/state/ForwardingInformationBaseContainer.h"
#include "fboss/agent/state/ForwardingInformationBaseMap.h"
#include "fboss/agent/state/NodeBase-defs.h"
#include "fboss/agent/state/NodeMap.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/SwitchState.h"

#include <folly/logging/xlog.h>

#include <algorithm>

namespace facebook::fboss::rib {

ForwardingInformationBaseUpdater::ForwardingInformationBaseUpdater(
    RouterID vrf,
    const IPv4NetworkToRouteMap& v4NetworkToRoute,
    const IPv6NetworkToRouteMap& v6NetworkToRoute)
    : vrf_(vrf),
      v4NetworkToRoute_(v4NetworkToRoute),
      v6NetworkToRoute_(v6NetworkToRoute) {}

std::shared_ptr<SwitchState> ForwardingInformationBaseUpdater::operator()(
    const std::shared_ptr<SwitchState>& state) {
  // A ForwardingInformationBaseContainer holds a
  // ForwardingInformationBaseV4 and a ForwardingInformationBaseV6 for a
  // particular VRF. Since FIBs for both address families will be updated,
  // we invoke modify() on the ForwardingInformationBaseContainer rather
  // than on its two children (namely ForwardingInformationBaseV4 and
  // ForwardingInformationBaseV6) in succession.
  // Unlike the coupled RIB implementation, we need only update the
  // SwitchState for a single VRF.
  auto previousFibContainer = state->getFibs()->getFibContainerIf(vrf_);
  CHECK(previousFibContainer);
  auto newFibV4 =
      createUpdatedFib(v4NetworkToRoute_, previousFibContainer->getFibV4());

  auto newFibV6 =
      createUpdatedFib(v6NetworkToRoute_, previousFibContainer->getFibV6());

  if (!newFibV4 && !newFibV6) {
    return state;
  }
  std::shared_ptr<SwitchState> nextState(state);
  auto nextFibContainer = previousFibContainer->modify(&nextState);

  if (newFibV4) {
    nextFibContainer->writableFields()->fibV4 = std::move(newFibV4);
  }

  if (newFibV6) {
    nextFibContainer->writableFields()->fibV6 = std::move(newFibV6);
  }
  return nextState;
}

template <typename AddressT>
std::shared_ptr<typename facebook::fboss::ForwardingInformationBase<AddressT>>
ForwardingInformationBaseUpdater::createUpdatedFib(
    const facebook::fboss::rib::NetworkToRouteMap<AddressT>& rib,
    const std::shared_ptr<facebook::fboss::ForwardingInformationBase<AddressT>>&
        fib) {
  typename facebook::fboss::ForwardingInformationBase<
      AddressT>::Base::NodeContainer updatedFib;

  bool updated = false;
  for (const auto& entry : rib) {
    const facebook::fboss::rib::Route<AddressT>& ribRoute = entry.value();

    if (!ribRoute.isResolved()) {
      // The recursive resolution algorithm considers a next-hop TO_CPU or
      // DROP to be resolved.
      continue;
    }

    // TODO(samank): optimize to linear time intersection algorithm
    facebook::fboss::RoutePrefix<AddressT> fibPrefix{
        ribRoute.prefix().network, ribRoute.prefix().mask};
    std::shared_ptr<facebook::fboss::Route<AddressT>> fibRoute =
        fib->getNodeIf(fibPrefix);
    if (fibRoute) {
      if (fibRoute->getClassID() == ribRoute.getClassID() &&
          toFibNextHop(ribRoute.getForwardInfo()) ==
              fibRoute->getForwardInfo()) {
        // Reuse prior FIB route
      } else {
        updated = true;
        fibRoute = toFibRoute(ribRoute, fibRoute);
      }
    } else {
      // new route
      updated = true;
      fibRoute = toFibRoute(ribRoute, fibRoute);
    }

    updatedFib.emplace_hint(updatedFib.cend(), fibPrefix, fibRoute);
  }
  // Check for deleted routes. Routes that were in the previous FIB
  // and have now been removed
  for (const auto& fibEntry : *fib) {
    const auto& prefix = fibEntry->prefix();
    if (rib.exactMatch(prefix.network, prefix.mask) == rib.end()) {
      updated = true;
      break;
    }
  }

  DCHECK_EQ(
      updatedFib.size(),
      std::count_if(
          rib.begin(),
          rib.end(),
          [](const typename std::remove_reference_t<decltype(
                 rib)>::ConstIterator::TreeNode& entry) {
            return entry.value().isResolved();
          }));

  return updated ? std::make_shared<ForwardingInformationBase<AddressT>>(
                       std::move(updatedFib))
                 : nullptr;
}

facebook::fboss::RouteNextHopEntry
ForwardingInformationBaseUpdater::toFibNextHop(
    const RouteNextHopEntry& ribNextHopEntry) {
  switch (ribNextHopEntry.getAction()) {
    case facebook::fboss::rib::RouteNextHopEntry::Action::DROP:
      return facebook::fboss::RouteNextHopEntry(
          facebook::fboss::RouteNextHopEntry::Action::DROP,
          ribNextHopEntry.getAdminDistance());
    case facebook::fboss::rib::RouteNextHopEntry::Action::TO_CPU:
      return facebook::fboss::RouteNextHopEntry(
          facebook::fboss::RouteNextHopEntry::Action::TO_CPU,
          ribNextHopEntry.getAdminDistance());
    case facebook::fboss::rib::RouteNextHopEntry::Action::NEXTHOPS: {
      facebook::fboss::RouteNextHopEntry::NextHopSet fibNextHopSet;
      for (const auto& ribNextHop : ribNextHopEntry.getNextHopSet()) {
        fibNextHopSet.insert(facebook::fboss::ResolvedNextHop(
            ribNextHop.addr(),
            ribNextHop.intfID().value(),
            ribNextHop.weight()));
      }
      return facebook::fboss::RouteNextHopEntry(
          fibNextHopSet, ribNextHopEntry.getAdminDistance());
    }
  }

  XLOG(FATAL) << "Unknown RouteNextHopEntry::Action value";
}

template <typename AddrT>
std::shared_ptr<facebook::fboss::Route<AddrT>>
ForwardingInformationBaseUpdater::toFibRoute(
    const Route<AddrT>& ribRoute,
    const std::shared_ptr<facebook::fboss::Route<AddrT>>& curFibRoute) {
  CHECK(ribRoute.isResolved());

  facebook::fboss::RoutePrefix<AddrT> fibPrefix;
  fibPrefix.network = ribRoute.prefix().network;
  fibPrefix.mask = ribRoute.prefix().mask;

  auto fibRoute = curFibRoute
      ? curFibRoute->clone()
      : std::make_shared<facebook::fboss::Route<AddrT>>(fibPrefix);

  fibRoute->setResolved(toFibNextHop(ribRoute.getForwardInfo()));
  if (ribRoute.isConnected()) {
    fibRoute->setConnected();
  }

  fibRoute->updateClassID(ribRoute.getClassID());
  return fibRoute;
}

template std::shared_ptr<facebook::fboss::Route<folly::IPAddressV4>>
ForwardingInformationBaseUpdater::toFibRoute<folly::IPAddressV4>(
    const Route<folly::IPAddressV4>&,
    const std::shared_ptr<facebook::fboss::Route<folly::IPAddressV4>>&);
template std::shared_ptr<facebook::fboss::Route<folly::IPAddressV6>>
ForwardingInformationBaseUpdater::toFibRoute<folly::IPAddressV6>(
    const Route<folly::IPAddressV6>&,
    const std::shared_ptr<facebook::fboss::Route<folly::IPAddressV6>>&);

} // namespace facebook::fboss::rib
