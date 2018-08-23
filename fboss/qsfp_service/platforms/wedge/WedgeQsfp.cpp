/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "WedgeQsfp.h"
#include <folly/Conv.h>
#include <folly/Memory.h>

#include <folly/logging/xlog.h>
#include "fboss/qsfp_service/StatsPublisher.h"

using namespace facebook::fboss;
using folly::MutableByteRange;
using std::make_unique;
using folly::StringPiece;
using std::unique_ptr;

namespace facebook { namespace fboss {

WedgeQsfp::WedgeQsfp(int module, TransceiverI2CApi* wedgeI2CBus)
    : module_(module), threadSafeI2CBus_(wedgeI2CBus) {
  moduleName_ = folly::to<std::string>(module);
}

WedgeQsfp::~WedgeQsfp() {
}

// Note that the module_ starts at 0, but the I2C bus module
// assumes that QSFP module numbers extend from 1 to 16.
//
bool WedgeQsfp::detectTransceiver() {
  return threadSafeI2CBus_->isPresent(module_ + 1);
}

int WedgeQsfp::readTransceiver(int dataAddress, int offset,
                               int len, uint8_t* fieldValue) {
  try {
    threadSafeI2CBus_->moduleRead(module_ + 1, dataAddress, offset, len,
                                  fieldValue);
  } catch (const I2cError& ex) {
    XLOG(ERR) << "Read from transceiver " << module_ << " at offset " << offset
              << " with length " << len << " failed: " << ex.what();
    StatsPublisher::bumpReadFailure();
    throw;
  }
  return len;
}

int WedgeQsfp::writeTransceiver(int dataAddress, int offset,
                            int len, uint8_t* fieldValue) {
  try {
    threadSafeI2CBus_->moduleWrite(module_ + 1, dataAddress, offset, len,
                                   fieldValue);
  } catch (const I2cError& ex) {
    XLOG(ERR) << "Write to transceiver " << module_ << " at offset " << offset
              << " with length " << len
              << " failed: " << folly::exceptionStr(ex);
    StatsPublisher::bumpWriteFailure();
    throw;
  }
  return len;
}

folly::StringPiece WedgeQsfp::getName() {
  return moduleName_;
}

int WedgeQsfp::getNum() const {
  return module_;
}

}}
