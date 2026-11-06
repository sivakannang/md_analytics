// mda-paperctl — sends paper orders to mda-broker for tests and replays (ADR-0029).
//
//   mda-paperctl --config config/replay.yaml place --user 1 --key "NSE|FO|FUT|NIFTY|2026-10-27"
//                --side BUY --qty 75 [--type MARKET|LIMIT|SL|SL-M] [--price P] [--trigger T]
//                [--product MIS|NRML]
//   mda-paperctl --config config/replay.yaml cancel --user 1 --order 12
//   mda-paperctl --config config/replay.yaml kill --user 1 on|off
//
// Writes one OrderRequest on the config's ord-gw bus and prints the execution reports that
// come back on exec within 3 seconds. Refuses the live bus "ord-gw": the gateway is its
// writer, and live paper orders come from the web app.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <mda/broker/messages.hpp>
#include <mda/bus/bus.hpp>
#include <mda/core/timestamp.hpp>
#include <mda/instruments/registry.hpp>
#include <mda/runtime/config.hpp>

namespace {

using namespace mda;

int usage() {
    std::fprintf(stderr,
                 "usage: mda-paperctl --config FILE place --user N --key KEY --side BUY|SELL --qty Q\n"
                 "                    [--type MARKET|LIMIT|SL|SL-M] [--price P] [--trigger T] [--product MIS|NRML]\n"
                 "       mda-paperctl --config FILE cancel --user N --order ID\n"
                 "       mda-paperctl --config FILE kill --user N on|off\n");
    return 2;
}

std::optional<core::Price> price_of(const std::map<std::string, std::string>& args, const char* name) {
    const auto found = args.find(name);
    if (found == args.end()) {
        return core::Price{};
    }
    return core::parse_price(found->second);
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/mda.yaml";
    std::string command;
    std::string kill_state;
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--") && i + 1 < argc) {
            (arg == "--config" ? config_path : args[std::string(arg.substr(2))]) = argv[++i];
        } else if (command.empty()) {
            command = arg;
        } else {
            kill_state = arg;
        }
    }
    const auto config = runtime::load_config(config_path);
    if (!config) {
        std::fprintf(stderr, "error: %s\n", config.error().c_str());
        return 1;
    }
    if (config->ord_gw_bus.name == "ord-gw") {
        std::fprintf(stderr, "error: refusing the live bus 'ord-gw'; use a test or replay config\n");
        return 1;
    }
    if (!args.contains("user")) {
        return usage();
    }

    broker::OrderRequest request{};
    request.at = core::Timestamp::now().ns() / 1'000'000;
    request.user = std::atoll(args["user"].c_str());
    request.client_order_id = static_cast<std::uint64_t>(request.at);  // unique enough for a tool
    request.source = oms::Source::Ui;
    if (command == "place") {
        const auto loaded = instruments::InstrumentRegistry::load(config->kite.registry_file);
        const auto key = instruments::parse_contract_key(args["key"]);
        const core::InstrumentId id = loaded && key ? loaded->registry.find(*key) : core::InstrumentId{};
        const auto price = price_of(args, "price");
        const auto trigger = price_of(args, "trigger");
        if (!id.valid() || !price || !trigger || !args.contains("side") || !args.contains("qty")) {
            std::fprintf(stderr, "error: unknown --key, or a bad --price / --trigger, or no --side / --qty\n");
            return usage();
        }
        const std::string type = args.contains("type") ? args["type"] : "MARKET";
        request.header = messages::make_header<broker::OrderRequest>(id, 1);
        request.action = broker::Action::New;
        request.side = args["side"] == "SELL" ? oms::Side::Sell : oms::Side::Buy;
        request.type = type == "LIMIT"  ? oms::OrderType::Limit
                       : type == "SL"   ? oms::OrderType::StopLimit
                       : type == "SL-M" ? oms::OrderType::StopMarket
                                        : oms::OrderType::Market;
        request.product = args["product"] == "NRML" ? oms::Product::Nrml : oms::Product::Mis;
        request.quantity = std::atoll(args["qty"].c_str());
        request.price = *price;
        request.trigger = *trigger;
        broker::set_text(request.tag, "paperctl");
    } else if (command == "cancel" && args.contains("order")) {
        request.header = messages::make_header<broker::OrderRequest>({}, 1);
        request.action = broker::Action::Cancel;
        request.order_id = std::atoll(args["order"].c_str());
    } else if (command == "kill" && (kill_state == "on" || kill_state == "off")) {
        request.header = messages::make_header<broker::OrderRequest>({}, 1);
        request.action = kill_state == "on" ? broker::Action::KillSwitchOn : broker::Action::KillSwitchOff;
    } else {
        return usage();
    }

    auto reports = bus::Reader::attach(config->exec_bus.name, {.name = "mda-paperctl", .register_slot = false});
    auto writer = bus::Writer::create_or_attach(
        {.name = config->ord_gw_bus.name, .capacity = config->ord_gw_bus.capacity_mb * 1024 * 1024});
    if (!writer) {
        std::fprintf(stderr, "error: bus %s: %s\n", config->ord_gw_bus.name.c_str(),
                     std::string(bus::to_string(writer.error())).c_str());
        return 1;
    }
    if (!reports) {
        (void)writer->publish(messages::to_wire(messages::MessageType::OrderRequest), messages::as_bytes(request));
        std::printf("sent (no exec bus yet: is mda-broker running with this config?)\n");
        return 0;
    }
    // Prints this user's reports; notes the answer to this request.
    bool answered = false;
    const auto show = [&](bus::MsgType type, std::span<const std::byte> payload) {
        const auto r = messages::decode<broker::ExecutionReport>(type, payload);
        if (!r || r->user != request.user) {
            return;
        }
        answered = answered || r->client_order_id == request.client_order_id;
        const std::string last =
            r->last_quantity > 0 ? std::format(" last {} @ {:.2f}", r->last_quantity, r->last_price.to_double()) : "";
        const std::string reason = broker::text_of(r->reason).empty() ? "" : " — " + std::string(broker::text_of(r->reason));
        std::printf("order %lld: event %d %s filled %lld/%lld%s%s\n", static_cast<long long>(r->order_id),
                    static_cast<int>(r->event), std::string(oms::name(r->status)).c_str(),
                    static_cast<long long>(r->filled), static_cast<long long>(r->quantity), last.c_str(),
                    reason.c_str());
    };
    const auto read_for = [&](std::chrono::milliseconds span, bool until_answered) {
        const auto until = std::chrono::steady_clock::now() + span;
        while (std::chrono::steady_clock::now() < until && !(until_answered && answered)) {
            if (reports->poll(show).status == bus::PollStatus::Empty) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    };
    // The bus is lossy (a reader attaching late misses earlier messages): send again, with
    // the same client order id, until mda-broker answers; a repeat is never a new order.
    for (int attempt = 1; attempt <= 3 && !answered; ++attempt) {
        (void)writer->publish(messages::to_wire(messages::MessageType::OrderRequest), messages::as_bytes(request));
        std::printf("sent: user %lld client %llu (try %d)\n", static_cast<long long>(request.user),
                    static_cast<unsigned long long>(request.client_order_id), attempt);
        read_for(std::chrono::seconds(1), true);
    }
    if (!answered) {
        std::printf("no answer from mda-broker\n");
        return 1;
    }
    read_for(std::chrono::seconds(3), false);  // fills that follow
    return 0;
}
