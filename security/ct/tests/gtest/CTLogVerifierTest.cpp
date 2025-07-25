/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CTLogVerifier.h"
#include "CTTestUtils.h"
#include "nss.h"
#include "signature_cache_ffi.h"

#include "gtest/gtest.h"

namespace mozilla {
namespace ct {

using namespace pkix;

class CTLogVerifierTest : public ::testing::Test {
 public:
  void SetUp() override {
    // Does nothing if NSS is already initialized.
    if (NSS_NoDB_Init(nullptr) != SECSuccess) {
      abort();
    }

    mSignatureCache = signature_cache_new(1);

    ASSERT_EQ(Success, mLog.Init(InputForBuffer(GetTestPublicKey())));
    ASSERT_EQ(GetTestPublicKeyId(), mLog.keyId());
  }

  void TearDown() override { signature_cache_free(mSignatureCache); }

 protected:
  CTLogVerifier mLog =
      CTLogVerifier(-1, CTLogState::Admissible, CTLogFormat::RFC6962, 0);
  // For some reason, the templating makes it impossible to use UniquePtr here.
  SignatureCache* mSignatureCache;
};

TEST_F(CTLogVerifierTest, VerifiesCertSCT) {
  LogEntry certEntry;
  GetX509CertLogEntry(certEntry);

  SignedCertificateTimestamp certSct;
  GetX509CertSCT(certSct);

  EXPECT_EQ(Success, mLog.Verify(certEntry, certSct, mSignatureCache));
}

TEST_F(CTLogVerifierTest, VerifiesPrecertSCT) {
  LogEntry precertEntry;
  GetPrecertLogEntry(precertEntry);

  SignedCertificateTimestamp precertSct;
  GetPrecertSCT(precertSct);

  EXPECT_EQ(Success, mLog.Verify(precertEntry, precertSct, mSignatureCache));
}

TEST_F(CTLogVerifierTest, FailsInvalidTimestamp) {
  LogEntry certEntry;
  GetX509CertLogEntry(certEntry);

  SignedCertificateTimestamp certSct;
  GetX509CertSCT(certSct);

  // Mangle the timestamp, so that it should fail signature validation.
  certSct.timestamp = 0;

  EXPECT_EQ(pkix::Result::ERROR_BAD_SIGNATURE,
            mLog.Verify(certEntry, certSct, mSignatureCache));
}

TEST_F(CTLogVerifierTest, FailsInvalidSignature) {
  LogEntry certEntry;
  GetX509CertLogEntry(certEntry);

  // Mangle the value of the signature, making the underlying signature
  // verification code return ERROR_BAD_SIGNATURE.
  SignedCertificateTimestamp certSct;
  GetX509CertSCT(certSct);
  certSct.signature.signatureData[20] ^= '\xFF';
  EXPECT_EQ(pkix::Result::ERROR_BAD_SIGNATURE,
            mLog.Verify(certEntry, certSct, mSignatureCache));

  // Mangle the encoding of the signature, making the underlying implementation
  // return ERROR_BAD_DER. We still expect the verifier to return
  // ERROR_BAD_SIGNATURE.
  SignedCertificateTimestamp certSct2;
  GetX509CertSCT(certSct2);
  certSct2.signature.signatureData[0] ^= '\xFF';
  EXPECT_EQ(pkix::Result::ERROR_BAD_SIGNATURE,
            mLog.Verify(certEntry, certSct2, mSignatureCache));
}

TEST_F(CTLogVerifierTest, FailsInvalidLogID) {
  LogEntry certEntry;
  GetX509CertLogEntry(certEntry);

  SignedCertificateTimestamp certSct;
  GetX509CertSCT(certSct);

  // Mangle the log ID, which should cause it to match a different log before
  // attempting signature validation.
  certSct.logId.push_back('\x0');

  EXPECT_EQ(pkix::Result::FATAL_ERROR_INVALID_ARGS,
            mLog.Verify(certEntry, certSct, mSignatureCache));
}

// Test that excess data after the public key is rejected.
TEST_F(CTLogVerifierTest, ExcessDataInPublicKey) {
  Buffer key = GetTestPublicKey();
  std::string extra = "extra";
  key.insert(key.end(), extra.begin(), extra.end());

  CTLogVerifier log(-1, CTLogState::Admissible, CTLogFormat::RFC6962, 0);
  EXPECT_NE(Success, log.Init(InputForBuffer(key)));
}

}  // namespace ct
}  // namespace mozilla
