#include "ludus/chess/chess.hpp"
#include "ludus/chess/game.hpp"
#include "ludus/chess/match.hpp"
#include "ludus/chess/pgn.hpp"
#include "ludus/chess/presentation.hpp"
#include "ludus/python/runtime.hpp"
#include "ludus/render/snapshot.hpp"
#include "ludus/render/theme.hpp"
#include "ludus/studio/package_document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<ludus::PythonRuntime> make_chess_runtime() {
    const std::vector<std::string> paths{
        std::string{LUDUS_SOURCE_DIR} + "/python",
        std::string{LUDUS_SOURCE_DIR} + "/games/orthodox_chess/python",
    };
    auto runtime = ludus::PythonRuntime::create(paths);
    INFO((runtime ? std::string{}
                  : runtime.error().message + "\n" + runtime.error().detail));
    REQUIRE(runtime);
    return std::move(*runtime);
}

class TemporaryPackage {
  public:
    TemporaryPackage() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ludus-chess-variation-" + std::to_string(nonce));
    }
    TemporaryPackage(const TemporaryPackage&) = delete;
    TemporaryPackage& operator=(const TemporaryPackage&) = delete;
    ~TemporaryPackage() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("orthodox chess initial position has the canonical perft sequence",
          "[chess][perft]") {
    const auto initial = ludus::chess::Position::initial();
    constexpr std::array<std::uint64_t, 5U> expected{20U, 400U, 8'902U, 197'281U,
                                                     4'865'609U};
    for (std::uint32_t depth = 1U; depth <= expected.size(); ++depth) {
        REQUIRE(ludus::chess::perft(initial, depth) == expected[depth - 1U]);
    }
}

TEST_CASE("orthodox chess emits canonical SAN without changing positions", "[chess][san]") {
    auto position = ludus::chess::Position::initial();
    for (const auto& [uci, san] : std::array{
             std::pair{"f2f3", "f3"}, std::pair{"e7e5", "e5"},
             std::pair{"g2g4", "g4"}, std::pair{"d8h4", "Qh4#"}}) {
        const auto move = position.find_legal_move(uci);
        REQUIRE(move);
        const auto notation = ludus::chess::to_san(position, *move);
        REQUIRE(notation);
        REQUIRE(*notation == san);
        REQUIRE(position.apply(*move));
    }
    const auto castle = ludus::chess::Position::from_fen(
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    REQUIRE(castle);
    const auto king_side = castle->find_legal_move("e1g1");
    REQUIRE(king_side);
    REQUIRE(ludus::chess::to_san(*castle, *king_side) == "O-O");
}

TEST_CASE("chess match clocks are deterministic across history and archives",
          "[chess][match][clock]") {
    auto runtime = make_chess_runtime();
    ludus::chess::ChessMatchSettings settings;
    settings.white_name = "Aurelia";
    settings.black_name = "Mordren";
    settings.time_control = {300'000, 2'000};
    auto created = ludus::chess::ChessMatch::create(*runtime, settings);
    REQUIRE(created);
    auto match = std::move(*created);

    REQUIRE(match.submit_uci("e2e4", 1'250));
    REQUIRE(match.remaining(ludus::chess::Color::white) == 300'750);
    REQUIRE(match.remaining(ludus::chess::Color::black) == 300'000);
    REQUIRE(match.submit_uci("e7e5", 3'000));
    REQUIRE(match.remaining(ludus::chess::Color::black) == 299'000);
    const auto committed_hash = match.match_hash();

    REQUIRE(match.undo());
    REQUIRE(match.remaining(ludus::chess::Color::white) == 300'750);
    REQUIRE(match.remaining(ludus::chess::Color::black) == 300'000);
    REQUIRE(match.redo());
    REQUIRE(match.remaining(ludus::chess::Color::black) == 299'000);
    REQUIRE(match.match_hash() == committed_hash);

    const auto archive = match.save();
    auto restored = ludus::chess::ChessMatch::load(*runtime, archive);
    REQUIRE(restored);
    REQUIRE(restored->match_hash() == committed_hash);
    REQUIRE(restored->game().state_hash() == match.game().state_hash());
    REQUIRE(restored->offer_draw(ludus::chess::Color::white));
    const auto offered_archive = ludus::chess::ChessMatchArchive::save(*restored);
    auto offered = ludus::chess::ChessMatchArchive::load(*runtime, offered_archive);
    REQUIRE(offered);
    REQUIRE(offered->draw_offer() == ludus::chess::Color::white);
    REQUIRE(offered->decline_draw(ludus::chess::Color::black));

    REQUIRE(match.undo());
    REQUIRE(match.submit_uci("c7c5", 4'000));
    REQUIRE(match.history_size() == 2U);
    REQUIRE_FALSE(match.redo());
}

TEST_CASE("chess match resolves flags, claims, and automatic outcomes",
          "[chess][match][result]") {
    auto runtime = make_chess_runtime();

    SECTION("flag fall is a draw when the opponent cannot possibly mate") {
        auto position = ludus::chess::Position::from_fen(
            "7k/8/8/8/8/8/8/Q3K3 w - - 0 1");
        REQUIRE(position);
        ludus::chess::ChessMatchSettings settings;
        settings.initial_position = *position;
        settings.time_control = {1'000, 0};
        auto created = ludus::chess::ChessMatch::create(*runtime, settings);
        REQUIRE(created);
        auto match = std::move(*created);
        REQUIRE(match.flag(ludus::chess::Color::white, 1'000));
        REQUIRE(match.result().outcome == ludus::chess::MatchOutcome::draw);
        REQUIRE(match.result().reason == ludus::chess::MatchResultReason::timeout);
        REQUIRE(match.result_elapsed_milliseconds() == 1'000);
        REQUIRE(match.undo());
        REQUIRE_FALSE(match.result().terminal());
        REQUIRE(match.remaining(ludus::chess::Color::white) == 1'000);
        const auto undone_archive = match.save();
        auto restored = ludus::chess::ChessMatch::load(*runtime, undone_archive);
        REQUIRE(restored);
        REQUIRE(restored->redo());
        REQUIRE(restored->result().reason == ludus::chess::MatchResultReason::timeout);
        REQUIRE(restored->result_elapsed_milliseconds() == 1'000);
        REQUIRE(restored->remaining(ludus::chess::Color::white) == 0);
    }

    SECTION("a lone minor can possibly mate when enemy material can block escape") {
        const auto position = ludus::chess::Position::from_fen(
            "7k/8/8/8/8/8/P7/2B1K3 b - - 0 1");
        REQUIRE(position);
        REQUIRE(ludus::chess::has_possible_mating_material(
            *position, ludus::chess::Color::white));
    }

    SECTION("flag fall awards a win when the opponent can mate") {
        ludus::chess::ChessMatchSettings settings;
        settings.time_control = {1'000, 0};
        auto created = ludus::chess::ChessMatch::create(*runtime, settings);
        REQUIRE(created);
        auto match = std::move(*created);
        REQUIRE(match.flag(ludus::chess::Color::white, 1'250));
        REQUIRE(match.result().outcome == ludus::chess::MatchOutcome::black_wins);
        REQUIRE(match.undo());
        REQUIRE(match.redo());
        REQUIRE(match.result().outcome == ludus::chess::MatchOutcome::black_wins);
        REQUIRE(match.result_elapsed_milliseconds() == 1'250);
    }

    SECTION("checkmate takes precedence over the seventy-five-move threshold") {
        auto position = ludus::chess::Position::from_fen(
            "7k/5Q2/6K1/8/8/8/8/8 w - - 149 1");
        REQUIRE(position);
        ludus::chess::ChessMatchSettings settings;
        settings.initial_position = *position;
        auto created = ludus::chess::ChessMatch::create(*runtime, settings);
        REQUIRE(created);
        auto match = std::move(*created);
        REQUIRE(match.submit_uci("f7g7"));
        REQUIRE(match.result().outcome == ludus::chess::MatchOutcome::white_wins);
        REQUIRE(match.result().reason == ludus::chess::MatchResultReason::checkmate);
    }

    SECTION("threefold can be claimed and fivefold is automatic") {
        auto created = ludus::chess::ChessMatch::create(*runtime);
        REQUIRE(created);
        auto match = std::move(*created);
        constexpr std::array cycle{"g1f3", "g8f6", "f3g1", "f6g8"};
        for (int repetition = 0; repetition < 2; ++repetition) {
            for (const auto move : cycle) {
                REQUIRE(match.submit_uci(move));
            }
        }
        REQUIRE(std::ranges::any_of(match.draw_claims(), [](const auto& claim) {
            return claim.reason == ludus::chess::MatchResultReason::threefold_repetition &&
                   !claim.intended_move;
        }));
        REQUIRE(match.claim_draw(ludus::chess::MatchResultReason::threefold_repetition));
        REQUIRE(match.result().outcome == ludus::chess::MatchOutcome::draw);

        REQUIRE(match.undo());
        for (int repetition = 0; repetition < 2; ++repetition) {
            for (const auto move : cycle) {
                REQUIRE(match.submit_uci(move));
            }
        }
        REQUIRE(match.result().reason == ludus::chess::MatchResultReason::fivefold_repetition);
    }

    SECTION("an intended quiet move can qualify for a fifty-move claim") {
        auto position = ludus::chess::Position::from_fen(
            "7k/8/8/8/8/8/8/R3K3 w - - 99 1");
        REQUIRE(position);
        ludus::chess::ChessMatchSettings settings;
        settings.initial_position = *position;
        auto created = ludus::chess::ChessMatch::create(*runtime, settings);
        REQUIRE(created);
        auto match = std::move(*created);
        const auto move = position->find_legal_move("a1a2");
        REQUIRE(move);
        REQUIRE(std::ranges::any_of(match.draw_claims(), [&](const auto& claim) {
            return claim.reason == ludus::chess::MatchResultReason::fifty_move_rule &&
                   claim.intended_move == move;
        }));
        REQUIRE(match.claim_draw(ludus::chess::MatchResultReason::fifty_move_rule, *move));
    }

    SECTION("an intended move can complete the third repetition") {
        auto created = ludus::chess::ChessMatch::create(*runtime);
        REQUIRE(created);
        auto match = std::move(*created);
        constexpr std::array first_cycle{"g1f3", "g8f6", "f3g1", "f6g8"};
        for (const auto move : first_cycle) {
            REQUIRE(match.submit_uci(move));
        }
        for (const auto move : std::array{"g1f3", "g8f6", "f3g1"}) {
            REQUIRE(match.submit_uci(move));
        }
        const auto intended = match.game().position()->find_legal_move("f6g8");
        REQUIRE(intended);
        REQUIRE(std::ranges::any_of(match.draw_claims(), [&](const auto& claim) {
            return claim.reason == ludus::chess::MatchResultReason::threefold_repetition &&
                   claim.intended_move == intended;
        }));
    }

    SECTION("repetition distinguishes castling rights and normalizes unusable en passant") {
        auto castling = ludus::chess::Position::from_fen(
            "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        REQUIRE(castling);
        ludus::chess::ChessMatchSettings castling_settings;
        castling_settings.initial_position = *castling;
        auto castling_created = ludus::chess::ChessMatch::create(*runtime, castling_settings);
        REQUIRE(castling_created);
        auto castling_match = std::move(*castling_created);
        constexpr std::array rook_cycle{"h1h2", "h8h7", "h2h1", "h7h8"};
        for (int repetition = 0; repetition < 2; ++repetition) {
            for (const auto move : rook_cycle) {
                REQUIRE(castling_match.submit_uci(move));
            }
        }
        REQUIRE(std::ranges::none_of(castling_match.draw_claims(), [](const auto& claim) {
            return claim.reason == ludus::chess::MatchResultReason::threefold_repetition &&
                   !claim.intended_move;
        }));
        for (const auto move : rook_cycle) {
            REQUIRE(castling_match.submit_uci(move));
        }
        REQUIRE(std::ranges::any_of(castling_match.draw_claims(), [](const auto& claim) {
            return claim.reason == ludus::chess::MatchResultReason::threefold_repetition &&
                   !claim.intended_move;
        }));

        auto phantom_ep = ludus::chess::Position::from_fen(
            "4k1n1/8/8/8/4P3/8/8/4K1N1 b - e3 0 1");
        REQUIRE(phantom_ep);
        ludus::chess::ChessMatchSettings ep_settings;
        ep_settings.initial_position = *phantom_ep;
        auto ep_created = ludus::chess::ChessMatch::create(*runtime, ep_settings);
        REQUIRE(ep_created);
        auto ep_match = std::move(*ep_created);
        constexpr std::array knight_cycle{"g8f6", "g1f3", "f6g8", "f3g1"};
        for (int repetition = 0; repetition < 2; ++repetition) {
            for (const auto move : knight_cycle) {
                REQUIRE(ep_match.submit_uci(move));
            }
        }
        REQUIRE(std::ranges::any_of(ep_match.draw_claims(), [](const auto& claim) {
            return claim.reason == ludus::chess::MatchResultReason::threefold_repetition &&
                   !claim.intended_move;
        }));
    }

    SECTION("terminal position rules and manual results are complete") {
        const auto outcome_for = [&](std::string_view fen) {
            const auto position = ludus::chess::Position::from_fen(fen);
            REQUIRE(position);
            ludus::chess::ChessMatchSettings settings;
            settings.initial_position = *position;
            auto created = ludus::chess::ChessMatch::create(*runtime, settings);
            REQUIRE(created);
            return std::move(*created);
        };
        auto mate = outcome_for("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
        REQUIRE(mate.result().reason == ludus::chess::MatchResultReason::checkmate);
        auto stalemate = outcome_for("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
        REQUIRE(stalemate.result().reason == ludus::chess::MatchResultReason::stalemate);
        auto dead = outcome_for("7k/8/8/8/8/8/8/2B1K3 w - - 0 1");
        REQUIRE(dead.result().reason == ludus::chess::MatchResultReason::insufficient_material);
        auto seventy_five = outcome_for("7k/8/8/8/8/8/8/R3K3 w - - 150 1");
        REQUIRE(seventy_five.result().reason ==
                ludus::chess::MatchResultReason::seventy_five_move_rule);

        auto resigned_created = ludus::chess::ChessMatch::create(*runtime);
        REQUIRE(resigned_created);
        auto resigned = std::move(*resigned_created);
        REQUIRE(resigned.resign(ludus::chess::Color::white));
        REQUIRE(resigned.result().outcome == ludus::chess::MatchOutcome::black_wins);
        REQUIRE(resigned.undo());
        REQUIRE(resigned.redo());
        REQUIRE(resigned.result().reason == ludus::chess::MatchResultReason::resignation);

        auto draw_created = ludus::chess::ChessMatch::create(*runtime);
        REQUIRE(draw_created);
        auto draw = std::move(*draw_created);
        REQUIRE(draw.agree_draw());
        REQUIRE(draw.result().reason == ludus::chess::MatchResultReason::agreed_draw);
    }
}

TEST_CASE("mainline PGN parses annotations and round trips deterministic clocks",
          "[chess][pgn]") {
    constexpr std::string_view source = R"PGN([Event "Reliquary Test"]
[Site "Local"]
[Date "2026.08.23"]
[Round "1"]
[White "Aurelia"]
[Black "Mordren"]
[Result "*"]
[TimeControl "300+2"]

1. e4 {A measured advance. [%clk 0:04:59]} $1 e5 {[%clk 0:04:58.500]}
2. Nf3 Nc6 *
)PGN";
    const auto parsed = ludus::chess::parse_pgn(source);
    INFO((parsed ? std::string{} : parsed.error().message));
    REQUIRE(parsed);
    REQUIRE(parsed->mainline.size() == 4U);
    REQUIRE(parsed->mainline.front().san == "e4");
    REQUIRE(parsed->mainline.front().nags == std::vector<std::uint32_t>{1U});
    REQUIRE(parsed->mainline.front().clock_remaining_milliseconds == 299'000);
    REQUIRE(parsed->settings.time_control == ludus::chess::TimeControl{300'000, 2'000});

    auto runtime = make_chess_runtime();
    auto imported = ludus::chess::import_pgn(*runtime, source);
    INFO((imported ? std::string{} : imported.error().message));
    REQUIRE(imported);
    REQUIRE(imported->history().size() == 4U);
    REQUIRE(imported->remaining(ludus::chess::Color::white) == 301'000);
    REQUIRE(imported->remaining(ludus::chess::Color::black) == 300'500);

    const auto exported = ludus::chess::export_pgn(*imported, "Reliquary Test", "Local");
    const auto reparsed = ludus::chess::parse_pgn(exported);
    REQUIRE(reparsed);
    REQUIRE(reparsed->mainline.size() == imported->history().size());
    REQUIRE(reparsed->settings.initial_position.to_fen() ==
            ludus::chess::Position::initial().to_fen());
}

TEST_CASE("chess match publishes structured immutable client records",
          "[chess][match][presentation][view]") {
    auto runtime = make_chess_runtime();
    ludus::chess::ChessMatchSettings settings;
    settings.white_name = "Aurelia";
    settings.black_name = "Mordren";
    settings.time_control = {300'000, 2'000};
    auto created = ludus::chess::ChessMatch::create(*runtime, settings);
    REQUIRE(created);
    auto match = std::move(*created);
    const auto presentation = ludus::chess::ChessPresentation::create(match.game());
    REQUIRE(presentation);

    auto initial = presentation->build_view(match, 1U);
    REQUIRE(initial);
    REQUIRE(initial->render.texts.size() == 16U);
    REQUIRE(initial->participants.size() == 2U);
    REQUIRE(initial->participants[0].name == "Aurelia");
    REQUIRE(initial->participants[0].active);
    REQUIRE(initial->clocks.size() == 2U);
    REQUIRE(initial->timeline.empty());
    REQUIRE(initial->text_exports.size() == 2U);
    REQUIRE(initial->text_exports[0].format == "FEN");
    REQUIRE(initial->text_exports[1].format == "PGN");

    const auto first = match.submit_uci("e2e4", 1'250);
    REQUIRE(first);
    REQUIRE(first->events);
    auto after = presentation->build_view(match, 2U, &initial->render, &*first->events);
    REQUIRE(after);
    REQUIRE(after->timeline.size() == 1U);
    REQUIRE(after->timeline.front().notation == "e4");
    REQUIRE(after->timeline.front().current);
    REQUIRE(after->participants[1].active);
    REQUIRE(after->clocks[0].committed_remaining_milliseconds == 300'750);
    REQUIRE(after->match_controls.can_undo);
    REQUIRE_FALSE(after->match_result);

    REQUIRE(match.submit_uci("d7d5"));
    REQUIRE(match.submit_uci("e4d5"));
    auto capture = presentation->build_view(match, 3U);
    REQUIRE(capture);
    REQUIRE(std::ranges::any_of(capture->captured_items, [](const auto& item) {
        return item.captured_from == ludus::ParticipantSide::second &&
               item.label == "Pawn" && item.count == 1;
    }));
}

TEST_CASE("PGN custom FEN is validated and variations have located diagnostics",
          "[chess][pgn][diagnostic]") {
    constexpr std::string_view custom = R"PGN([Event "Castle"]
[Site "Local"]
[Date "????.??.??"]
[Round "-"]
[White "Ivory"]
[Black "Iron"]
[Result "*"]
[SetUp "1"]
[FEN "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"]

1. O-O O-O-O *
)PGN";
    const auto parsed = ludus::chess::parse_pgn(custom);
    REQUIRE(parsed);
    REQUIRE(parsed->mainline.size() == 2U);

    const auto variation = ludus::chess::parse_pgn(
        "[Result \"*\"]\n\n1. e4 (1. d4) e5 *\n");
    REQUIRE_FALSE(variation);
    REQUIRE(variation.error().line == 3U);
    REQUIRE(variation.error().column > 1U);
    REQUIRE(variation.error().message.find("variations") != std::string::npos);

    const auto annotations = ludus::chess::parse_pgn(
        "[Result \"*\"]\n\n{prologue} 1. e4!? $2 e5 *\n");
    REQUIRE(annotations);
    REQUIRE(annotations->leading_comments == std::vector<std::string>{"prologue"});
    REQUIRE(annotations->mainline.front().nags == std::vector<std::uint32_t>{5U, 2U});

    const auto malformed = ludus::chess::parse_pgn(
        "[Result \"*\"]\n\n1. e5 *\n");
    REQUIRE_FALSE(malformed);
    REQUIRE(malformed.error().line == 3U);
    REQUIRE(malformed.error().column == 4U);

    const auto malformed_fen = ludus::chess::parse_pgn(
        "[SetUp \"1\"]\n[FEN \"not-a-fen\"]\n[Result \"*\"]\n\n*\n");
    REQUIRE_FALSE(malformed_fen);
    REQUIRE(malformed_fen.error().line == 2U);
    REQUIRE(malformed_fen.error().column == 1U);
    REQUIRE(malformed_fen.error().message.find("FEN") != std::string::npos);

    const std::string oversized((1U << 20U) + 1U, ' ');
    const auto bounded = ludus::chess::parse_pgn(oversized);
    REQUIRE_FALSE(bounded);
    REQUIRE(bounded.error().message.find("1 MiB") != std::string::npos);
}

TEST_CASE("PGN result imports preserve deterministic termination semantics",
          "[chess][pgn][result]") {
    auto runtime = make_chess_runtime();
    ludus::chess::ChessMatchSettings clocked_settings;
    clocked_settings.time_control = {1'000, 0};
    auto clocked_created = ludus::chess::ChessMatch::create(*runtime, clocked_settings);
    REQUIRE(clocked_created);
    auto clocked = std::move(*clocked_created);
    REQUIRE(clocked.flag(ludus::chess::Color::white, 1'000));
    const auto timeout_pgn = ludus::chess::export_pgn(clocked);
    auto timeout_import = ludus::chess::import_pgn(*runtime, timeout_pgn);
    REQUIRE(timeout_import);
    REQUIRE(timeout_import->result().reason == ludus::chess::MatchResultReason::timeout);
    REQUIRE(timeout_import->result().outcome == ludus::chess::MatchOutcome::black_wins);

    auto repeated_created = ludus::chess::ChessMatch::create(*runtime);
    REQUIRE(repeated_created);
    auto repeated = std::move(*repeated_created);
    constexpr std::array cycle{"g1f3", "g8f6", "f3g1", "f6g8"};
    for (int repetition = 0; repetition < 2; ++repetition) {
        for (const auto move : cycle) {
            REQUIRE(repeated.submit_uci(move));
        }
    }
    REQUIRE(repeated.claim_draw(ludus::chess::MatchResultReason::threefold_repetition));
    auto repetition_import = ludus::chess::import_pgn(
        *runtime, ludus::chess::export_pgn(repeated));
    REQUIRE(repetition_import);
    REQUIRE(repetition_import->result().reason ==
            ludus::chess::MatchResultReason::threefold_repetition);
}

TEST_CASE("orthodox chess package theme resolves both generated piece sets",
          "[chess][assets][theme]") {
    const auto root = std::filesystem::path{LUDUS_SOURCE_DIR} / "games/orthodox_chess";
    const auto theme = ludus::VisualTheme::load_package(root);
    INFO((theme ? std::string{} : theme.error().message + "\n" + theme.error().detail));
    REQUIRE(theme);
    REQUIRE(theme->sprites().size() == 16U);
    REQUIRE(theme->sprite("piece.ivory.pawn") == ludus::SpriteId{0U});
    REQUIRE(theme->sprite("piece.iron.king") == ludus::SpriteId{11U});
    REQUIRE(theme->sprite("participant.ivory.crest") == ludus::SpriteId{12U});
    REQUIRE(theme->sprite("decoration.frame.corner") == ludus::SpriteId{14U});
    REQUIRE(theme->sprite("board.material.dark") == ludus::SpriteId{15U});
    REQUIRE(theme->color("interaction.capture").has_value());
    REQUIRE(theme->atlas().regions().size() == 16U);
}

TEST_CASE("orthodox chess package passes focused reference perft positions",
          "[chess][perft][regression]") {
    const auto kiwipete = ludus::chess::Position::from_fen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    REQUIRE(kiwipete);
    REQUIRE(ludus::chess::perft(*kiwipete, 1U) == 48U);
    REQUIRE(ludus::chess::perft(*kiwipete, 2U) == 2'039U);
    REQUIRE(ludus::chess::perft(*kiwipete, 3U) == 97'862U);

    const auto endgame = ludus::chess::Position::from_fen(
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    REQUIRE(endgame);
    REQUIRE(ludus::chess::perft(*endgame, 1U) == 14U);
    REQUIRE(ludus::chess::perft(*endgame, 2U) == 191U);
    REQUIRE(ludus::chess::perft(*endgame, 3U) == 2'812U);
}

TEST_CASE("orthodox chess FEN round trips and rejects malformed positions", "[chess][fen]") {
    constexpr std::string_view fen =
        "r3k2r/p1ppqpb1/bn2pnp1/2pP4/1p2P3/2N2N2/PPQBBPPP/R3K2R w KQkq - 0 1";
    const auto position = ludus::chess::Position::from_fen(fen);
    REQUIRE(position);
    REQUIRE(position->to_fen() == fen);
    REQUIRE_FALSE(ludus::chess::Position::from_fen("8/8/8/8/8/8/8/8 w - - 0 1"));
}

TEST_CASE("castling, en passant, promotion, mate, and stalemate are legal-state rules",
          "[chess][special-rules]") {
    const auto castling = ludus::chess::Position::from_fen(
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    REQUIRE(castling);
    REQUIRE(castling->find_legal_move("e1g1"));
    REQUIRE(castling->find_legal_move("e1c1"));

    const auto en_passant =
        ludus::chess::Position::from_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    REQUIRE(en_passant);
    const auto ep_move = en_passant->find_legal_move("e5d6");
    REQUIRE(ep_move);
    REQUIRE(ludus::chess::has_flag(ep_move->flags, ludus::chess::MoveFlag::en_passant));

    const auto promotion =
        ludus::chess::Position::from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    REQUIRE(promotion);
    REQUIRE(promotion->find_legal_move("a7a8q"));
    REQUIRE(promotion->find_legal_move("a7a8r"));
    REQUIRE(promotion->find_legal_move("a7a8b"));
    REQUIRE(promotion->find_legal_move("a7a8n"));
    REQUIRE_FALSE(promotion->find_legal_move("a7a8"));

    const auto mate =
        ludus::chess::Position::from_fen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
    const auto stalemate =
        ludus::chess::Position::from_fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    REQUIRE(mate);
    REQUIRE(stalemate);
    REQUIRE(mate->status() == ludus::chess::PositionStatus::checkmate);
    REQUIRE(stalemate->status() == ludus::chess::PositionStatus::stalemate);
}

TEST_CASE("orthodox chess runs through authoritative sessions and Python transactions",
          "[chess][session][python]") {
    auto runtime = make_chess_runtime();
    auto game_result = ludus::chess::ChessGame::create(*runtime);
    INFO((game_result ? std::string{}
                      : game_result.error().message + "\n" + game_result.error().detail));
    REQUIRE(game_result);
    auto game = std::move(*game_result);

    const auto moves = game.legal_moves();
    REQUIRE(moves);
    REQUIRE(moves->size() == 20U);
    REQUIRE(game.session().legal_actions(ludus::PlayerId{0U, 1U}).size() == 20U);
    REQUIRE(game.session().legal_actions(ludus::PlayerId{1U, 1U}).empty());
    REQUIRE_FALSE(game.submit(ludus::chess::ChessMove{12U, 36U}));
    REQUIRE_FALSE(game.submit(ludus::chess::ChessMove{255U, 0U}));

    REQUIRE(game.submit_uci("e2e4"));
    REQUIRE(game.submit_uci("e7e5"));
    REQUIRE(game.submit_uci("g1f3"));
    REQUIRE(game.position()->to_fen() ==
            "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
    REQUIRE(game.move_history().size() == 3U);
    const auto replayed = game.replayed_state_hash();
    REQUIRE(replayed);
    REQUIRE(*replayed == game.state_hash());

    const auto three_ply_hash = game.state_hash();
    REQUIRE(game.undo());
    REQUIRE(game.position()->to_fen() ==
            "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2");
    REQUIRE(game.redo());
    REQUIRE(game.state_hash() == three_ply_hash);
    REQUIRE(game.undo());
    REQUIRE(game.submit_uci("f1c4"));
    REQUIRE(game.move_history().size() == 3U);
    REQUIRE(ludus::chess::to_uci(game.move_history().back()) == "f1c4");
    REQUIRE_FALSE(game.redo());
    REQUIRE(game.replayed_state_hash() == game.state_hash());
}

TEST_CASE("chess-like package rules reload atomically at a safe session boundary",
          "[chess][studio][reload]") {
    TemporaryPackage temporary;
    auto document = ludus::studio::PackageDocument::create(
        temporary.path(), "org.example.short-rook");
    REQUIRE(document);
    const std::vector<std::string> paths{
        std::string{LUDUS_SOURCE_DIR} + "/python", temporary.path().string()};
    auto runtime_result = ludus::PythonRuntime::create(paths);
    REQUIRE(runtime_result);
    auto runtime = std::move(*runtime_result);
    const auto position = ludus::chess::Position::from_fen(
        "4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    REQUIRE(position);
    auto game_result = ludus::chess::ChessGame::create(
        *runtime, *position, document->manifest().entry_point);
    REQUIRE(game_result);
    auto game = std::move(*game_result);
    const auto has_move = [&game](std::string_view expected) {
        const auto moves = game.legal_moves();
        return moves && std::ranges::any_of(*moves, [expected](const auto move) {
                   return ludus::chess::to_uci(move) == expected;
               });
    };
    REQUIRE(has_move("a1a8"));
    const auto state_hash = game.state_hash();

    auto source = document->python_source();
    const std::string original =
        "\"rook\": move.rays(ORTHOGONAL).until_blocked().allow_empty().capture_enemy()";
    const std::string replacement =
        "\"rook\": move.jumps(ORTHOGONAL).allow_empty().capture_enemy()";
    const auto rook_rule = source.find(original);
    REQUIRE(rook_rule != std::string::npos);
    source.replace(rook_rule, original.size(), replacement);
    document->set_python_source(source);
    REQUIRE(document->save());
    REQUIRE(game.reload_rules());
    REQUIRE(runtime->generation() == 2U);
    REQUIRE(game.state_hash() == state_hash);
    REQUIRE_FALSE(has_move("a1a8"));
    REQUIRE(has_move("a1a2"));

    source = document->python_source();
    const auto king_rule = source.find("    \"king\":");
    REQUIRE(king_rule != std::string::npos);
    const auto king_rule_end = source.find('\n', king_rule);
    source.erase(king_rule, king_rule_end - king_rule + 1U);
    document->set_python_source(std::move(source));
    REQUIRE(document->save());
    const auto rejected = game.reload_rules();
    REQUIRE_FALSE(rejected);
    REQUIRE(runtime->generation() == 2U);
    REQUIRE(game.state_hash() == state_hash);
    REQUIRE(has_move("a1a2"));
}

TEST_CASE("chess presentation produces immutable selectable snapshots and animations",
          "[chess][presentation][render]") {
    auto runtime = make_chess_runtime();
    auto game = ludus::chess::ChessGame::create(*runtime);
    REQUIRE(game);
    const auto presentation = ludus::chess::ChessPresentation::create(*game);
    REQUIRE(presentation);
    const auto atlas = ludus::chess::make_default_chess_atlas();
    REQUIRE(atlas);
    REQUIRE(atlas->regions().size() == 12U);

    const auto start = std::chrono::steady_clock::time_point{};
    auto before = presentation->build(*game, 1U, nullptr, nullptr, start);
    REQUIRE(before);
    REQUIRE(before->spaces.size() == 64U);
    REQUIRE(before->pieces.size() == 32U);
    REQUIRE(before->actions.size() == 20U);
    REQUIRE(before->texts.size() == 16U);
    REQUIRE(before->static_revision != before->dynamic_revision);
    REQUIRE(before->status == "White to move — 20 legal moves");
    REQUIRE(ludus::pick_space(*before, {-3.5F, -3.5F}) == ludus::SpaceId{0U, 1U});

    const auto e2e4 = std::ranges::find_if(before->actions, [](const ludus::ActionHint& hint) {
        const auto move = ludus::chess::decode_action_token(hint.token);
        return move.from == 12U && move.to == 28U;
    });
    REQUIRE(e2e4 != before->actions.end());
    const auto decoded = ludus::chess::decode_action_token(e2e4->token);
    REQUIRE(decoded.from == 12U);
    REQUIRE(decoded.to == 28U);

    const auto committed = game->submit(decoded);
    REQUIRE(committed);
    auto after = presentation->build(*game, 2U, &*before, &*committed, start);
    REQUIRE(after);
    REQUIRE(after->animations.size() == 1U);
    REQUIRE(after->animations.front().entity == e2e4->actor);
    const auto* moved_piece = ludus::find_piece(*after, e2e4->actor);
    REQUIRE(moved_piece != nullptr);
    REQUIRE(moved_piece->location == ludus::SpaceId{28U, 1U});
    REQUIRE(ludus::animated_center(*after, *moved_piece,
                                   start + std::chrono::milliseconds{90}) ==
            ludus::Vec2{0.5F, -1.5F});
}

TEST_CASE("session transactions preserve castling, en passant, and promotion semantics",
          "[chess][session][special-rules]") {
    auto runtime = make_chess_runtime();

    SECTION("castling moves both entities") {
        const auto position = ludus::chess::Position::from_fen(
            "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
        REQUIRE(position);
        auto game = ludus::chess::ChessGame::create(*runtime, *position);
        REQUIRE(game);
        REQUIRE(game->submit_uci("e1g1"));
        REQUIRE(game->position()->to_fen() ==
                "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1");
    }

    SECTION("en passant destroys the bypassed pawn") {
        const auto position = ludus::chess::Position::from_fen(
            "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
        REQUIRE(position);
        auto game = ludus::chess::ChessGame::create(*runtime, *position);
        REQUIRE(game);
        REQUIRE(game->submit_uci("e5d6"));
        REQUIRE(game->position()->to_fen() ==
                "4k3/8/3P4/8/8/8/8/4K3 b - - 0 1");
    }

    SECTION("promotion changes the authoritative entity type") {
        const auto position = ludus::chess::Position::from_fen(
            "4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
        REQUIRE(position);
        auto game = ludus::chess::ChessGame::create(*runtime, *position);
        REQUIRE(game);
        REQUIRE(game->submit_uci("a7a8n"));
        REQUIRE(game->position()->to_fen() ==
                "N3k3/8/8/8/8/8/8/4K3 b - - 0 1");
    }

    SECTION("terminal status is exposed by the session adapter") {
        const auto mate = ludus::chess::Position::from_fen(
            "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
        REQUIRE(mate);
        auto mate_game = ludus::chess::ChessGame::create(*runtime, *mate);
        REQUIRE(mate_game);
        REQUIRE(mate_game->status() == ludus::chess::PositionStatus::checkmate);

        const auto stalemate = ludus::chess::Position::from_fen(
            "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
        REQUIRE(stalemate);
        auto stalemate_game = ludus::chess::ChessGame::create(*runtime, *stalemate);
        REQUIRE(stalemate_game);
        REQUIRE(stalemate_game->status() == ludus::chess::PositionStatus::stalemate);
    }
}
