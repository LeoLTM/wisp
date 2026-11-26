#pragma once

#include "sendspin/client.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/// Stores sendspin-cpp's persistent state as one file per key in a state
/// directory (default /var/lib/wispd).
///
/// The library uses it for the device identity (the X25519 keypair the
/// client_id is derived from), the pairing records of every paired server, the
/// Pairing PSK, the last-played server and the output delay.  Without it the
/// device gets a new identity on every start and loses its pairings.
///
/// The files hold secrets, so each one is written owner-only (0600) through a
/// temp file that is fsync'd and renamed into place, making every write atomic
/// and durable.  The library calls every method on the main loop thread, so no
/// locking is needed.
class FilePersistence : public sendspin::SendspinPersistenceProvider {
  public:
    explicit FilePersistence(std::string dir);

    /// Create the state directory (0700) if it does not exist.  Returns false
    /// and prints an error if it cannot be created or is not a directory.
    bool init();

    const std::string& dir() const { return dir_; }

    std::optional<size_t> read_blob(std::string_view key, std::span<uint8_t> out) override;
    bool write_blob(std::string_view key, std::span<const uint8_t> data) override;

  private:
    /// Path of the file holding key, or empty if key is not a plain file name.
    std::string path_for(std::string_view key) const;

    std::string dir_;
};
