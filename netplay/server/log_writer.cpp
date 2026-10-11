#include "log_writer.hpp"
#include "cpp_yyjson.hpp"
#include <fstream>
#include <iostream>

namespace f3rt::netplay {

LogWriter::LogWriter(std::filesystem::path log_dir)
    : log_dir_(std::move(log_dir)) {}

std::string LogWriter::serialize_snapshot_json(const SessionSnapshot &snap) {
    yyjson::object j;
    j.emplace("session_id", snap.session_id_hex, yyjson::copy_string);
    j.emplace("started_at", snap.started_at, yyjson::copy_string);
    j.emplace("ended_at", snap.ended_at, yyjson::copy_string);
    j.emplace("game_id", snap.game_id, yyjson::copy_string);
    j.emplace("build_hash", snap.build_hash_hex, yyjson::copy_string);

    yyjson::array rom_crcs;
    for (uint32_t crc : snap.rom_crcs) {
        rom_crcs.emplace_back(crc);
    }
    j.emplace("rom_crcs", std::move(rom_crcs));

    j.emplace("seed", snap.seed_hex, yyjson::copy_string);
    j.emplace("input_delay", unsigned(snap.input_delay));

    yyjson::array player_names;
    player_names.emplace_back(snap.player_names[0], yyjson::copy_string);
    player_names.emplace_back(snap.player_names[1], yyjson::copy_string);
    j.emplace("player_names", std::move(player_names));

    // Inputs: array of 2 arrays, dense by frame from frame 0 (-1 is null)
    yyjson::array inputs_arr;
    for (int p = 0; p < 2; ++p) {
        yyjson::array p_inputs;
        for (int32_t val : snap.inputs[p]) {
            if (val < 0) {
                p_inputs.emplace_back(nullptr);
            } else {
                p_inputs.emplace_back(val);
            }
        }
        inputs_arr.emplace_back(std::move(p_inputs));
    }
    j.emplace("inputs", std::move(inputs_arr));

    // CRC
    yyjson::object crc_obj;
    crc_obj.emplace("compared", snap.crc_compared);
    crc_obj.emplace("mismatch_count", snap.crc_mismatch_count);
    yyjson::array mismatches_arr;
    for (const auto &m : snap.crc_mismatches) {
        yyjson::object m_obj;
        m_obj.emplace("frame", m.frame);
        m_obj.emplace("crc_p1", m.crc_p1);
        m_obj.emplace("crc_p2", m.crc_p2);
        mismatches_arr.emplace_back(std::move(m_obj));
    }
    crc_obj.emplace("mismatches", std::move(mismatches_arr));
    crc_obj.emplace("unmatched", snap.crc_unmatched);
    crc_obj.emplace("evicted", snap.crc_evicted);
    j.emplace("crc", std::move(crc_obj));

    // Results: array of 2 entries (object or null)
    yyjson::array results_arr;
    for (int p = 0; p < 2; ++p) {
        if (snap.results[p].has_value()) {
            const auto &r = *snap.results[p];
            yyjson::object r_obj;
            r_obj.emplace("frame", r.frame);
            r_obj.emplace("winner", unsigned(r.winner));
            r_obj.emplace("final_crc", r.final_crc);
            results_arr.emplace_back(std::move(r_obj));
        } else {
            results_arr.emplace_back(nullptr);
        }
    }
    j.emplace("results", std::move(results_arr));

    // Result Ack
    if (snap.result_ack.has_value()) {
        yyjson::object ack_obj;
        ack_obj.emplace("agreed", snap.result_ack->agreed);
        ack_obj.emplace("winner", unsigned(snap.result_ack->winner));
        j.emplace("result_ack", std::move(ack_obj));
    } else {
        j.emplace("result_ack", nullptr);
    }

    // Sync Blob
    if (snap.sync_blob_hex.has_value()) {
        j.emplace("sync_blob", *snap.sync_blob_hex, yyjson::copy_string);
    } else {
        j.emplace("sync_blob", nullptr);
    }

    // Protocol errors
    yyjson::array errs_arr;
    for (const auto &err : snap.protocol_errors) {
        errs_arr.emplace_back(err, yyjson::copy_string);
    }
    j.emplace("protocol_errors", std::move(errs_arr));
    j.emplace("protocol_errors_dropped", snap.protocol_errors_dropped);

    j.emplace("end_reason", snap.end_reason, yyjson::copy_string);
    j.emplace("ended_by", snap.ended_by);

    // Chat
    yyjson::array chat_arr;
    for (const auto &c : snap.chat) {
        yyjson::object c_obj;
        c_obj.emplace("player", unsigned(c.player));
        c_obj.emplace("t_ms", c.t_ms);
        c_obj.emplace("text", c.text, yyjson::copy_string);
        chat_arr.emplace_back(std::move(c_obj));
    }
    j.emplace("chat", std::move(chat_arr));

    yyjson::array chat_dropped_arr;
    chat_dropped_arr.emplace_back(snap.chat_dropped[0]);
    chat_dropped_arr.emplace_back(snap.chat_dropped[1]);
    j.emplace("chat_dropped", std::move(chat_dropped_arr));

    const auto text = j.write(yyjson::WriteFlag::Pretty | yyjson::WriteFlag::PrettyTwoSpaces |
                              yyjson::WriteFlag::NewlineAtEnd);
    return std::string(text);
}

bool LogWriter::write_log(const SessionSnapshot &snap) const {
    std::error_code ec;
    std::filesystem::create_directories(log_dir_, ec);
    if (ec) {
        return false;
    }

    std::filesystem::path target_path = log_dir_ / (snap.session_id_hex + ".json");
    std::filesystem::path tmp_path = log_dir_ / (snap.session_id_hex + ".json.tmp");
    std::string json_data = serialize_snapshot_json(snap);

    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return false;
        }
        ofs.write(json_data.data(), static_cast<std::streamsize>(json_data.size()));
        if (!ofs.good()) {
            return false;
        }
        ofs.flush();
    }

    std::filesystem::rename(tmp_path, target_path, ec);
    return !ec;
}

} // namespace f3rt::netplay
