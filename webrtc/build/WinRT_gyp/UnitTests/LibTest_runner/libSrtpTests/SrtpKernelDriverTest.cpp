/*
*  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
*
*  Use of this source code is governed by a BSD-style license
*  that can be found in the LICENSE file in the root of the source
*  tree. An additional intellectual property rights grant can be found
*  in the file PATENTS.  All contributing project authors may
*  be found in the AUTHORS file in the root of the source tree.
*/

#include "common.h"
#include "SrtpKernelDriverTest.h"

// test entry point declaration
extern "C" int srtp_test_kernel_driver_main(int argc, char *argv[]);

AUTO_ADD_TEST_IMPL(libSrtpTests::CSrtpKernelDriverTest);

int libSrtpTests::CSrtpKernelDriverTest::InterchangeableExecute() {
  // TODO: configuration has to be handled
  char* argv[] = { ".", "-v" };
  return srtp_test_kernel_driver_main(2, argv);
}

