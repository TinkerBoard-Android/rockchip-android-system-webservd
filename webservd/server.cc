// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "webservd/server.h"

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <limits>

#include <base/rand_util.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/async_event_sequencer.h>

#include "webservd/dbus_protocol_handler.h"
#include "webservd/protocol_handler.h"
#include "webservd/utils.h"

using chromeos::dbus_utils::AsyncEventSequencer;
using chromeos::dbus_utils::DBusObject;
using chromeos::dbus_utils::ExportedObjectManager;

namespace {

void OnFirewallSuccess(const std::string& itf_name,
                       uint16_t port,
                       bool allowed) {
  if (allowed) {
    LOG(INFO) << "Successfully opened up port " << port << " on interface "
              << itf_name;
  } else {
    LOG(ERROR) << "Failed to open up port " << port << ", interface: "
               << itf_name;
  }
}

void IgnoreFirewallDBusMethodError(chromeos::Error* error) {
}

}  // anonymous namespace

namespace webservd {

Server::Server(ExportedObjectManager* object_manager, const Config& config,
               std::unique_ptr<FirewallInterface> firewall)
    : dbus_object_{new DBusObject{
          object_manager, object_manager->GetBus(),
          org::chromium::WebServer::ServerAdaptor::GetObjectPath()}},
      config_{config},
      firewall_{std::move(firewall)} {}

Server::~Server() {}

void Server::RegisterAsync(
    const AsyncEventSequencer::CompletionAction& completion_callback) {
  scoped_refptr<AsyncEventSequencer> sequencer(new AsyncEventSequencer());
  dbus_adaptor_.RegisterWithDBusObject(dbus_object_.get());

  InitTlsData();

  for (auto& handler_config : config_.protocol_handlers)
    CreateProtocolHandler(&handler_config);

  firewall_->WaitForServiceAsync(dbus_object_->GetBus().get(),
                                 base::Bind(&Server::OnFirewallServiceOnline,
                                            weak_ptr_factory_.GetWeakPtr()));

  dbus_object_->RegisterAsync(
      sequencer->GetHandler("Failed exporting Server.", true));

  for (const auto& pair : protocol_handler_map_) {
    pair.second->RegisterAsync(
        sequencer->GetHandler("Failed exporting ProtocolHandler.", false));
  }
  sequencer->OnAllTasksCompletedCall({completion_callback});
}

void Server::OnFirewallServiceOnline() {
  LOG(INFO) << "Firewall service is on-line. "
            << "Opening firewall for protocol handlers";
  for (auto& handler_config : config_.protocol_handlers) {
    VLOG(1) << "Firewall request: Protocol Handler = " << handler_config.name
            << ", Port = " << handler_config.port << ", Interface = "
            << handler_config.interface_name;
    firewall_->PunchTcpHoleAsync(
        handler_config.port,
        handler_config.interface_name,
        base::Bind(&OnFirewallSuccess, handler_config.interface_name,
                   handler_config.port),
        base::Bind(&IgnoreFirewallDBusMethodError));
  }
}

std::string Server::Ping() {
  return "Web Server is running";
}

void Server::ProtocolHandlerStarted(ProtocolHandler* handler) {
  CHECK(protocol_handler_map_.find(handler) == protocol_handler_map_.end())
      << "Protocol handler already registered";
  std::string path = base::StringPrintf("/org/chromium/WebServer/Servers/%d",
                                        ++last_protocol_handler_index_);
  dbus::ObjectPath object_path{path};
  std::unique_ptr<DBusProtocolHandler> dbus_protocol_handler{
      new DBusProtocolHandler{dbus_object_->GetObjectManager().get(),
                              object_path,
                              handler,
                              this}
  };
  protocol_handler_map_.emplace(handler, std::move(dbus_protocol_handler));
}

void Server::ProtocolHandlerStopped(ProtocolHandler* handler) {
  CHECK_EQ(1u, protocol_handler_map_.erase(handler))
      << "Unknown protocol handler";
}

void Server::CreateProtocolHandler(Config::ProtocolHandler* handler_config) {
  std::unique_ptr<ProtocolHandler> protocol_handler{
      new ProtocolHandler{handler_config->name, this}};
  if (protocol_handler->Start(handler_config))
    protocol_handlers_.push_back(std::move(protocol_handler));
}

void Server::InitTlsData() {
  if (!TLS_certificate_.empty())
    return;  // Already initialized.

  // TODO(avakulenko): verify these constants and provide sensible values
  // for the long-term. See brbug.com/227
  const int kKeyLengthBits = 1024;
  const base::TimeDelta kCertExpiration = base::TimeDelta::FromDays(365);
  const char kCommonName[] = "Brillo device";

  // Create the X509 certificate.
  int cert_serial_number = base::RandInt(0, std::numeric_limits<int>::max());
  auto cert =
      CreateCertificate(cert_serial_number, kCertExpiration, kCommonName);

  // Create RSA key pair.
  auto rsa_key_pair = GenerateRSAKeyPair(kKeyLengthBits);

  // Store the private key to a temp buffer.
  // Do not assign it to |TLS_private_key_| yet until the end when we are sure
  // everything else has worked out.
  chromeos::SecureBlob private_key = StoreRSAPrivateKey(rsa_key_pair.get());

  // Create EVP key and set it to the certificate.
  auto key = std::unique_ptr<EVP_PKEY, void (*)(EVP_PKEY*)>{EVP_PKEY_new(),
                                                            EVP_PKEY_free};
  CHECK(key.get());
  // Transfer ownership of |rsa_key_pair| to |key|.
  CHECK(EVP_PKEY_assign_RSA(key.get(), rsa_key_pair.release()));
  CHECK(X509_set_pubkey(cert.get(), key.get()));

  // Sign the certificate.
  CHECK(X509_sign(cert.get(), key.get(), EVP_sha256()));

  TLS_certificate_ = StoreCertificate(cert.get());
  TLS_certificate_fingerprint_ = GetSha256Fingerprint(cert.get());
  TLS_private_key_ = std::move(private_key);

  // Update the TLS data in protocol handler config.
  for (auto& handler_config : config_.protocol_handlers) {
    if (handler_config.use_tls) {
      handler_config.certificate = TLS_certificate_;
      handler_config.certificate_fingerprint = TLS_certificate_fingerprint_;
      handler_config.private_key = TLS_private_key_;
    }
  }
}

}  // namespace webservd