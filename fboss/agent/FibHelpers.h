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

#include "fboss/agent/state/ForwardingInformationBase.h"
#include "fboss/agent/state/ForwardingInformationBaseContainer.h"
#include "fboss/agent/state/ForwardingInformationBaseDelta.h"
#include "fboss/agent/state/Route.h"
#include "fboss/agent/state/RouteTable.h"
#include "fboss/agent/state/RouteTableMap.h"
#include "fboss/agent/state/RouteTableRib.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/types.h"

#include "fboss/agent/state/StateDelta.h"

#include <folly/IPAddress.h>

#include <memory>

namespace facebook::fboss {

class SwitchState;

template <typename AddrT>
std::shared_ptr<Route<AddrT>> findRoute(
    bool isStandaloneRib,
    RouterID rid,
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<SwitchState>& state);

template <typename AddrT>
std::shared_ptr<Route<AddrT>> findLongestMatchRoute(
    bool isStandaloneRib,
    RouterID rid,
    const AddrT& addr,
    const std::shared_ptr<SwitchState>& state);

template <typename Func>
void forAllRoutes(
    bool isStandaloneRib,
    const std::shared_ptr<SwitchState>& state,
    Func& func) {
  if (isStandaloneRib) {
    for (const auto& fibContainer : *state->getFibs()) {
      auto rid = fibContainer->getID();
      for (const auto& route : *(fibContainer->getFibV6())) {
        func(rid, route);
      }
      for (const auto& route : *(fibContainer->getFibV4())) {
        func(rid, route);
      }
    }
  } else {
    for (const auto& routeTable : *state->getRouteTables()) {
      auto rid = routeTable->getID();
      for (const auto& route : *(routeTable->getRibV6()->routes())) {
        func(rid, route);
      }
      for (const auto& route : *(routeTable->getRibV4()->routes())) {
        func(rid, route);
      }
    }
  }
}

template <
    typename AddrT,
    typename ChangedFn,
    typename AddFn,
    typename RemoveFn,
    typename... Args>
void forEachChangedRoute(
    bool isStandaloneRib,
    const StateDelta& stateDelta,
    ChangedFn changedFn,
    AddFn addedFn,
    RemoveFn removedFn,
    const Args&... args) {
  auto removeAll = [&](RouterID rid, const auto& routes) {
    for (const auto& oldRoute : routes) {
      removedFn(args..., rid, oldRoute);
    }
  };
  auto processRoutesDelta = [&](RouterID rid, const auto& routesDelta) {
    for (auto const& routeDelta : routesDelta) {
      auto const& oldRoute = routeDelta.getOld();
      auto const& newRoute = routeDelta.getNew();

      if (!oldRoute) {
        addedFn(args..., rid, newRoute);
      } else if (!newRoute) {
        removedFn(args..., rid, oldRoute);
      } else {
        changedFn(args..., rid, oldRoute, newRoute);
      }
    }
  };
  if (isStandaloneRib) {
    for (const auto& fibContainerDelta : stateDelta.getFibsDelta()) {
      auto const& newFibContainer = fibContainerDelta.getNew();
      if (!newFibContainer) {
        auto const& oldFibContainer = fibContainerDelta.getOld();
        removeAll(
            oldFibContainer->getID(),
            *oldFibContainer->template getFib<AddrT>());
        continue;
      }
      processRoutesDelta(
          newFibContainer->getID(), fibContainerDelta.getFibDelta<AddrT>());
    }

  } else {
    for (auto const& routeTableDelta : stateDelta.getRouteTablesDelta()) {
      auto const& newRouteTable = routeTableDelta.getNew();
      if (!newRouteTable) {
        auto const& oldRouteTable = routeTableDelta.getOld();
        removeAll(
            oldRouteTable->getID(),
            *oldRouteTable->template getRib<AddrT>()->routes());
        continue;
      }
      processRoutesDelta(
          newRouteTable->getID(), routeTableDelta.getRoutesDelta<AddrT>());
    }
  }
}

template <
    typename ChangedFn,
    typename AddFn,
    typename RemoveFn,
    typename... Args>
void forEachChangedRoute(
    bool isStandaloneRib,
    const StateDelta& delta,
    ChangedFn changedFn,
    AddFn addedFn,
    RemoveFn removedFn,
    const Args&... args) {
  forEachChangedRoute<folly::IPAddressV4>(
      isStandaloneRib, delta, changedFn, addedFn, removedFn, args...);
  forEachChangedRoute<folly::IPAddressV6>(
      isStandaloneRib, delta, changedFn, addedFn, removedFn, args...);
}
} // namespace facebook::fboss
