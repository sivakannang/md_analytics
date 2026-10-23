#include <array>
#include <cstdio>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <mda/notify/codes.hpp>

namespace mda::notify {
namespace {

using storage::DbError;
using storage::StepResult;

std::vector<std::byte> sha256(std::string_view text) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    EVP_Digest(text.data(), text.size(), digest.data(), &length, EVP_sha256(), nullptr);
    std::vector<std::byte> out(length);
    for (unsigned int i = 0; i < length; ++i) {
        out[i] = static_cast<std::byte>(digest[i]);
    }
    return out;
}

std::string email_material(std::int64_t user_id, std::string_view code) {
    return std::to_string(user_id) + ":" + std::string(code);
}

std::expected<void, DbError> store(storage::Database& db, std::int64_t user_id, std::string_view purpose,
                                   const std::vector<std::byte>& hash, std::int64_t expires_at) {
    auto transaction = storage::Transaction::begin(db);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }
    auto drop = db.prepare("DELETE FROM channel_codes WHERE user_id = ?1 AND purpose = ?2");
    if (!drop) {
        return std::unexpected(drop.error());
    }
    drop->bind(1, user_id).bind(2, purpose);
    if (auto done = drop->run(); !done) {
        return std::unexpected(done.error());
    }
    auto insert = db.prepare("INSERT INTO channel_codes (code_hash, user_id, purpose, expires_at) VALUES (?1, ?2, ?3, ?4)");
    if (!insert) {
        return std::unexpected(insert.error());
    }
    insert->bind(1, std::span<const std::byte>(hash)).bind(2, user_id).bind(3, purpose).bind(4, expires_at);
    if (auto done = insert->run(); !done) {
        return std::unexpected(done.error());
    }
    return transaction->commit();
}

/// The owner of an unexpired code with this hash and purpose, deleting it.
std::expected<std::optional<std::int64_t>, DbError> consume(storage::Database& db, std::string_view purpose,
                                                            const std::vector<std::byte>& hash, std::int64_t now) {
    auto query = db.prepare(
        "DELETE FROM channel_codes WHERE code_hash = ?1 AND purpose = ?2 AND expires_at > ?3 RETURNING user_id");
    if (!query) {
        return std::unexpected(query.error());
    }
    query->bind(1, std::span<const std::byte>(hash)).bind(2, purpose).bind(3, now);
    const auto row = query->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (*row == StepResult::Done) {
        return std::optional<std::int64_t>{};
    }
    return std::optional<std::int64_t>{query->column_int64(0)};
}

}  // namespace

std::expected<std::string, DbError> create_link_code(storage::Database& db, std::int64_t user_id,
                                                     std::chrono::seconds ttl, std::int64_t now) {
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return std::unexpected(DbError{.code = 0, .message = "no random bytes"});
    }
    std::string code;
    for (const unsigned char b : random) {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", b);
        code += hex;
    }
    if (auto stored = store(db, user_id, kTelegramLink, sha256(code), now + ttl.count()); !stored) {
        return std::unexpected(stored.error());
    }
    return code;
}

std::expected<std::optional<std::int64_t>, DbError> consume_link_code(storage::Database& db, std::string_view code,
                                                                      std::int64_t now) {
    return consume(db, kTelegramLink, sha256(code), now);
}

std::expected<std::optional<std::string>, DbError> create_email_code(storage::Database& db, std::int64_t user_id,
                                                                     std::int64_t now) {
    auto recent = db.prepare(
        "SELECT 1 FROM channel_codes WHERE user_id = ?1 AND purpose = ?2 AND expires_at - ?3 > ?4");
    if (!recent) {
        return std::unexpected(recent.error());
    }
    recent->bind(1, user_id).bind(2, kEmailVerify).bind(3, kEmailCodeTtl.count()).bind(4, now - kEmailCodeGap.count());
    const auto row = recent->step();
    if (!row) {
        return std::unexpected(row.error());
    }
    if (*row == StepResult::Row) {
        return std::optional<std::string>{};  // asked again too soon
    }
    std::uint32_t random = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&random), sizeof random) != 1) {
        return std::unexpected(DbError{.code = 0, .message = "no random bytes"});
    }
    char code[8];
    std::snprintf(code, sizeof code, "%06u", random % 1'000'000);
    if (auto stored = store(db, user_id, kEmailVerify, sha256(email_material(user_id, code)), now + kEmailCodeTtl.count());
        !stored) {
        return std::unexpected(stored.error());
    }
    return std::optional<std::string>(code);
}

std::expected<bool, DbError> consume_email_code(storage::Database& db, std::int64_t user_id, std::string_view code,
                                                std::int64_t now) {
    auto owner = consume(db, kEmailVerify, sha256(email_material(user_id, code)), now);
    if (!owner) {
        return std::unexpected(owner.error());
    }
    if (owner->has_value()) {
        return true;
    }
    // A wrong guess: count it, and drop the code at the limit.
    auto count = db.prepare("UPDATE channel_codes SET attempts = attempts + 1 WHERE user_id = ?1 AND purpose = ?2");
    auto drop = db.prepare("DELETE FROM channel_codes WHERE user_id = ?1 AND purpose = ?2 AND attempts >= ?3");
    if (!count || !drop) {
        return std::unexpected(!count ? count.error() : drop.error());
    }
    count->bind(1, user_id).bind(2, kEmailVerify);
    drop->bind(1, user_id).bind(2, kEmailVerify).bind(3, static_cast<std::int64_t>(kEmailCodeGuesses));
    if (auto done = count->run(); !done) {
        return std::unexpected(done.error());
    }
    if (auto done = drop->run(); !done) {
        return std::unexpected(done.error());
    }
    return false;
}

}  // namespace mda::notify
