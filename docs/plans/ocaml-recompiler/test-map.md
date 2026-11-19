# Python recompiler test mapping

One row per test function, including parametrized functions. Python paths are
relative to `tests/recomp/` and OCaml paths are relative to `recomp/test/`.
`ported` identifies a case added or extended in T20. `covered-by` identifies
existing equivalent checks. `stays-with-tools` cases belong to the decision
comparison tool and continue to run in Python.

113 functions: 38 ported, 66 covered-by, 6 dropped-with-reason, 3 stays-with-tools.

| Python file and test | OCaml file and case, or reason | Status |
| --- | --- | --- |
| `test_abi_version.py::test_header_abi_version_matches_generator` | `generated/cases.ml` — generated header matches runtime | ported |
| `test_block_profile.py::ProfileTests.test_union_keeps_rom_identities_and_saturates_counts` | `selection/profile.ml` — merge, identities and saturation | covered-by |
| `test_block_profile.py::ProfileTests.test_miss_is_not_hot_and_rom_identity_must_match` | `selection/profile.ml` — matching identity and miss filtering | covered-by |
| `test_block_profile.py::ProfileTests.test_exclusions_reject_matching_hits_and_misses_but_not_other_roms` | `selection/profile.ml` — exclusions on both CPUs | covered-by |
| `test_block_profile.py::ProfileTests.test_rejects_invalid_addresses_counts_versions_and_missing_identity` | `selection/profile.ml` — strict input | covered-by |
| `test_block_profile.py::ProfileTests.test_rejects_merge_of_conflicting_bounds` | `selection/profile.ml` — merge, identities and saturation | covered-by |
| `test_block_profile_runtime.py::RuntimeProfileTests.test_relative_destination_stays_fixed_and_merges_after_cwd_change` | `generated/cases.ml` — persistence across cwd changes | ported |
| `test_decision_diff.py::test_decoder_diff_reports_explained_and_unexplained_cells` | `tools/recomp_compare` decision diff | stays-with-tools |
| `test_decision_diff.py::test_decoder_diff_audits_missing_ordinary_rows` | `tools/recomp_compare` decision diff | stays-with-tools |
| `test_decision_diff.py::test_discovery_diff_explains_masked_target_but_rejects_wrong_chain` | `tools/recomp_compare` decision diff | stays-with-tools |
| `test_discovery.py::ActorDiscoveryTests.test_long_callback_and_cyclic_script_calls` | D3 removed recursive actor-script discovery and callback seeds. | dropped-with-reason |
| `test_discovery.py::ActorDiscoveryTests.test_register_staged_script_counter_records` | D3 removed recursive actor-script discovery and callback seeds. | dropped-with-reason |
| `test_discovery.py::ActorDiscoveryTests.test_explicit_script_table_count_excludes_adjacent_data` | D3 removed recursive actor-script discovery and callback seeds. | dropped-with-reason |
| `test_discovery.py::ActorDiscoveryTests.test_staged_jump_tables_with_backward_destinations` | `discover/aligned.ml` — staged tables and signed displacement | ported |
| `test_discovery.py::ActorDiscoveryTests.test_signed_full_extension_table_displacement` | `discover/aligned.ml` — staged tables and signed displacement | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_computed_target_with_no_pointer_literal` | `discover/aligned.ml` — computed, odd-offset, long and overlapping entries | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_odd_offset_24bit_pointer_target` | `discover/aligned.ml` — computed, odd-offset, long and overlapping entries | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_greater_than_32_straight_line_instructions_without_filter` | `discover/aligned.ml` — computed, odd-offset, long and overlapping entries | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_overlapping_starts` | `discover/aligned.ml` — computed, odd-offset, long and overlapping entries | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_odd_pcs_and_truncated_instruction_boundary` | `discover/aligned.ml` — computed, odd-offset, long and overlapping entries; odd configured entry | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_computed_jump_table_targets_are_codegen_only` | `discover/aligned.ml` — computed candidates and static edges | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_exhaustive_explicit_jump_table_overrides_scanner` | `discover/aligned.ml` — explicit table overrides scanning | ported |
| `test_discovery.py::AllAlignedDiscoveryTests.test_recursive_behavior_retained` | D3 removed recursive discovery. The `config_test.ml` case recursive discovery checks rejection of that mode. | dropped-with-reason |
| `test_discovery.py::ExclusionTests.test_half_open_interval_and_candidate_accounting` | `discover/aligned.ml` — exclusion accounting and dispatch boundary | ported |
| `test_discovery.py::ExclusionTests.test_vector_and_explicit_table_targets_reject_exclusions` | `discover/aligned.ml` — excluded vector and configured targets | ported |
| `test_discovery.py::ExclusionTests.test_unproven_direct_transfer_is_reported_for_runtime_enforcement` | `discover/aligned.ml` — exclusion accounting and dispatch boundary. The old JSON transfer report was removed. | ported |
| `test_discovery.py::ExclusionTests.test_recursive_reachable_exclusion_is_not_silently_dropped` | D3 removed recursive discovery. The `config_test.ml` case recursive discovery checks rejection of that mode. | dropped-with-reason |
| `test_discovery.py::ExclusionTests.test_overlap_odd_bounds_and_missing_evidence_reject` | `config_test.ml` — odd bounds; interval outside ROM; empty evidence; overlap | covered-by |
| `test_discovery.py::ExclusionContractTests.test_aliased_24_bit_bus_target_is_excluded` | `config_test.ml` — matching fill and 24-bit alias | covered-by |
| `test_discovery.py::ExclusionContractTests.test_fill_matching_image_passes` | `config_test.ml` — matching fill and 24-bit alias | covered-by |
| `test_discovery.py::ExclusionContractTests.test_fill_contradicted_by_image_fails` | `config_test.ml` — contradicted fill | covered-by |
| `test_discovery.py::ExclusionContractTests.test_fill_honours_cpu_base_and_requires_image` | `config_test.ml` — matching fill and 24-bit alias; contradicted sound fill; fill without image | covered-by |
| `test_discovery.py::ExclusionContractTests.test_fill_must_be_a_byte` | `config_test.ml` — invalid fill 256; invalid fill -1; invalid fill true; invalid fill 1.5 | covered-by |
| `test_discovery.py::LoadRomUnknownKeyTests.test_known_keys_load` | `rom_manifest_test.ml` — known main ROM keys load | ported |
| `test_discovery.py::LoadRomUnknownKeyTests.test_unknown_rom_key_names_file_and_key` | `config_test.ml` — unknown rom key | covered-by |
| `test_discovery.py::LoadRomUnknownKeyTests.test_unknown_lane_key_names_file_and_key` | `config_test.ml` — unknown lane key | covered-by |
| `test_discovery.py::LoadRomUnknownKeyTests.test_every_game_config_uses_only_known_rom_keys` | `config_test.ml` — all shipped configs | covered-by |
| `test_discovery.py::LoadRomUnknownKeyTests.test_landmakrj_ff_fills_match_real_program_images` | `config_test.ml` — shipped main and sound fills | ported |
| `test_enhancements.py::test_parse_spec_and_render_header` | `config_test.ml` — resolve holds and render header | covered-by |
| `test_enhancements.py::test_runtime_only_ignores_holds` | `config_test.ml` — default ignores holds | covered-by |
| `test_enhancements.py::test_invalid_counter` | `config_test.ml` — invalid counter true; invalid counter 0x3ffffe; invalid counter 0x420000; invalid counter 0x408329 | covered-by |
| `test_enhancements.py::test_duplicate_name` | `config_test.ml` — duplicate hold name | covered-by |
| `test_enhancements.py::test_invalid_hold` | `config_test.ml` — equals in name; period below range; period above range; unknown follow mode; unknown group writer; unknown anchor writer | covered-by |
| `test_enhancements.py::test_base_mismatch` | `config_test.ml` — base mismatch | covered-by |
| `test_generate.py::DeadlineTests.test_deadline_yields_with_materialized_flags_and_resumes_inside_block` | `generated/cases.ml` — deadline flags and resumption | ported |
| `test_generate.py::DeadlineTests.test_overlapping_entries_skip_extensions_and_preserve_deadline_resumption` | `generated/cases.ml` — overlapping entries | ported |
| `test_generate.py::DeadlineTests.test_cmpm_consumes_eeprom_signature_and_aliased_operands` | `generated/cases.ml` — CMPM operand order and aliasing | ported |
| `test_generate.py::BlockChainingTests.test_backward_loop_chains_in_one_call_with_unchained_step_count` | `generated/cases.ml` — backward loop and step count | ported |
| `test_generate.py::BlockChainingTests.test_chain_yields_at_first_deadline_boundary_and_resumes_identically` | `generated/cases.ml` — chain deadline and resumption | ported |
| `test_generate.py::BlockChainingTests.test_cross_function_transfer_chains_but_trace_refuses` | `generated/cases.ml` — cross-function transfer and trace | ported |
| `test_generate.py::BlockChainingTests.test_unretained_chain_target_returns_to_dispatcher` | `generated/cases.ml` — unretained target returns | ported |
| `test_generate.py::IndirectChainTests.test_table_indexes_chain_in_one_call_with_dispatcher_step_count` | `generated/cases.ml` — indexed and staged chains | ported |
| `test_generate.py::IndirectChainTests.test_unknown_index_and_trace_refuse_the_chain` | `generated/cases.ml` — unknown index and traced jump | ported |
| `test_generate.py::FlagLivenessTests.test_producer_overwritten_before_any_read_is_elided` | `liveness/cases.ml` — overwrite elides producer | covered-by |
| `test_generate.py::FlagLivenessTests.test_producer_feeding_a_conditional_branch_is_kept` | `liveness/cases.ml` — conditional branch reads Z; flagless move preserves Z | covered-by |
| `test_generate.py::FlagLivenessTests.test_only_x_live_after_add_keeps_eager_x_and_clears_pending_state` | `liveness/cases.ml` — ADD keeps eager X and clears stale pending state | covered-by |
| `test_generate.py::FlagLivenessTests.test_x_only_add_is_not_clobbered_by_stale_pending_state` | `liveness/cases.ml` — X survives pending flags in the runtime | covered-by |
| `test_generate.py::FlagLivenessTests.test_partial_writers_that_preserve_x_keep_an_earlier_x_producer` | `liveness/cases.ml` — compare preserves X; logic preserves X | covered-by |
| `test_generate.py::FlagLivenessTests.test_boundaries_keep_every_flag_live` | `liveness/cases.ml` — RTS boundary; computed JMP boundary; missing fallthrough; known BRA; missing BSR destination | covered-by |
| `test_generate.py::FlagLivenessTests.test_known_call_flows_into_callee_and_not_to_the_return_point` | `liveness/cases.ml` — known call enters callee | covered-by |
| `test_generate.py::FlagLivenessTests.test_loop_back_edge_reaches_fixpoint` | `liveness/cases.ml` — loop back edge | covered-by |
| `test_generate.py::FlagLivenessTests.test_pinned_pcs_observe_every_flag` | `liveness/cases.ml` — pin observes all | covered-by |
| `test_generate.py::FlagLivenessTests.test_reaching_writers` | The legacy reaching-writers query has no OCaml counterpart. `liveness/cases.ml` checks the overlapping CFG behavior in both branch predecessors supply the shared tail. | dropped-with-reason |
| `test_generate.py::FlagLivenessTests.test_effects_follow_the_instruction_semantics` | `liveness/cases.ml` — effects follow IR semantics | covered-by |
| `test_generate.py::FlagLivenessTests.test_lowering_never_stores_dead_flags_and_effects_cover_what_lower_emits` | `liveness/cases.ml` — producer effects and lowering over every opcode | covered-by |
| `test_generate.py::FlagLivenessEquivalenceTests.test_elided_and_exact_flags_agree_from_any_entry_and_pending_state` | `refinement/cases.ml` — straight line; forward branches; runtime and control boundaries | covered-by |
| `test_idle.py::IdleLoopShapeTests.test_recognises_the_three_shapes` | `idle/cases.ml` — recognises spin, poll and affine | covered-by |
| `test_idle.py::IdleLoopShapeTests.test_instructions_that_are_not_plain_native_code_keep_their_loop_unmodified` | `idle/cases.ml` — requires plain native code | covered-by |
| `test_idle.py::IdleLoopShapeTests.test_rejects_counted_polls` | `idle/cases.ml` — rejects counted polls | covered-by |
| `test_idle.py::IdleLoopShapeTests.test_rejects_polls_whose_address_can_change_inside_the_loop` | `idle/cases.ml` — rejects changing, indexed and PC-relative EAs | covered-by |
| `test_idle.py::IdleLoopEquivalenceTests.test_yield_stops_where_plain_execution_would_be_after_the_same_steps` | `generated/cases.ml` — idle yields preserve state at the same step | ported |
| `test_inline_memory.py::InlineMemoryTests.test_inline_access_matches_the_callbacks` | `t14/memory.c` — `memory.exe` runtest rule in `t14/dune` | covered-by |
| `test_profile_codegen.py::CodegenProfileTests.test_cross_tier_successors_deadlines_indirect_entries_and_slim_removal` | `generated/cases.ml` — tier transitions, deadlines and saturation | ported |
| `test_profile_codegen.py::CodegenProfileTests.test_main_shared_exception_entries_remain_counted_once_in_each_mode` | `generated/cases.ml` — shared exceptions count once | ported |
| `test_profile_codegen.py::CodegenProfileTests.test_main_exclusions_preserve_tier_complement_and_slim_exception_subset` | `generated/cases.ml` — excluded ranges and tier complements | ported |
| `test_profile_codegen.py::CodegenProfileTests.test_main_rejects_excluded_discovery_and_profile_records_before_emission` | `selection/selection.ml` — exception subsets, exclusions and RAM; `selection/profile.ml` — exclusions on both CPUs | ported |
| `test_roms.py::RomRegionTests.test_size_zero_region_is_absent_plane_not_a_chip_file` | `rom_manifest_test.ml` — grouped graphics and sparse samples; `config_test.ml` — chip in absent region | ported |
| `test_roms.py::RomRegionTests.test_grouped_tile_lanes_and_sparse_zero_samples` | `rom_manifest_test.ml` — grouped graphics and sparse samples | ported |
| `test_roms.py::RomRegionTests.test_sound_mirror_keeps_both_banks_and_rejects_corruption` | `rom_manifest_test.ml` — grouped sparse fill and mirror; corrupted mirrored chip | covered-by |
| `test_roms.py::RomRegionTests.test_continue_then_patch_preserves_gseeker_lane_order` | `rom_manifest_test.ml` — continue and ordered overwrite | covered-by |
| `test_roms.py::RomRegionTests.test_continue_validates_bytes_outside_selected_source_slice` | `rom_manifest_test.ml` — corruption outside source slice | covered-by |
| `test_roms.py::RomRegionTests.test_only_hash_validated_short_dumps_are_ff_padded` | `rom_manifest_test.ml` — validated short dump pads with 0xff; corrupt short dump; short SHA1 mismatch; padded short dump must match full hash | covered-by |
| `test_roms.py::RomRegionTests.test_group_cannot_write_past_region` | `config_test.ml` — group exceeds region | covered-by |
| `test_sprite_trace.py::LandmakrjTraceTests.test_exact_writer_table` | `sprite/trace_test.ml` — landmakrj / writer table and rigidity | covered-by |
| `test_sprite_trace.py::LandmakrjTraceTests.test_scaled_handler_is_not_rigid` | `sprite/trace_test.ml` — landmakrj / writer table and rigidity | covered-by |
| `test_sprite_trace.py::LandmakrjTraceTests.test_coverage` | `sprite/trace_test.ml` — landmakrj / coverage | covered-by |
| `test_sprite_trace.py::LandmakrjTraceTests.test_continuity_classification` | `sprite/trace_test.ml` — landmakrj / continuity | covered-by |
| `test_sprite_trace.py::CommandwTraceTests.test_origin_writer_table` | `sprite/trace_test.ml` — commandw / writer table | covered-by |
| `test_sprite_trace.py::CommandwTraceTests.test_continuity_classification` | `sprite/trace_test.ml` — commandw / continuity | covered-by |
| `test_sprite_trace.py::CommandwTraceTests.test_unresolved_writers_are_reported` | `sprite/trace_test.ml` — commandw / coverage | covered-by |
| `test_sprite_trace.py::HeaderTests.test_renders_and_parses` | `sprite/trace_test.ml` — landmakrj / C++20 header | ported |
| `test_sprite_trace.py::HeaderTests.test_compiles_with_cxx20` | `sprite/trace_test.ml` — landmakrj / C++20 header | ported |
| `test_sprite_trace.py::HeaderTests.test_empty_table_uses_empty_spans` | `sprite/trace_test.ml` — synthetic / empty header | ported |
| `test_sprite_trace.py::FixtureTests.test_branch_union_cursor_and_lane_offsets` | `sprite/trace_test.ml` — synthetic / branch and cursor union | covered-by |
| `test_sprite_trace.py::FixtureTests.test_explicit_writer_seed_forces_lane` | `sprite/trace_test.ml` — synthetic / forced lane | covered-by |
| `test_sprite_trace.py::FixtureTests.test_generate_writes_sprite_groups` | `emit/cases.ml` — sprite artifacts from emit | ported |
| `test_sprite_trace.py::WriterSeedTests.test_array_and_object_forms` | `sprite/trace_test.ml` — synthetic / writer seed JSON | covered-by |
| `test_sprite_units.py::ParseTests.test_defaults_and_scalar_lists` | `config_test.ml` — defaults, scalar and list PCs, hook maps | covered-by |
| `test_sprite_units.py::ParseTests.test_empty_config` | `config_test.ml` — empty headers | covered-by |
| `test_sprite_units.py::ParseTests.test_errors` | `config_test.ml` — sprite units validation cases | covered-by |
| `test_sprite_units.py::DigestTests.test_stable_and_sensitive` | `config_test.ml` — digest parity and sensitivity | covered-by |
| `test_sprite_units.py::DigestTests.test_digest_independent_of_unrelated_config` | `config_test.ml` — digest parity and sensitivity | covered-by |
| `test_sprite_units.py::DigestTests.test_digest_ignores_retired_frame_writer_table` | `config_test.ml` — digest parity and sensitivity | covered-by |
| `test_sprite_units.py::DigestTests.test_pinned_value` | `config_test.ml` — digest parity and sensitivity | covered-by |
| `test_sprite_units.py::RenderTests.test_empty` | `config_test.ml` — empty headers | covered-by |
| `test_sprite_units.py::RenderTests.test_units` | `config_test.ml` — populated headers | covered-by |
| `test_sprite_units.py::RenderTests.test_leftover_frame_writer_table_is_ignored` | `config_test.ml` — retired frame writers ignored | covered-by |
| `test_sprite_units.py::RenderTests.test_guard_lines` | `config_test.ml` — digest guard | covered-by |
| `test_sprite_units.py::RenderTests.test_shipped_configs_parse` | `config_test.ml` — shipped unit names | covered-by |
| `test_sprite_units.py::GeneratorHookTests.test_hooks_at_labels` | `generated/cases.ml` — hook order and empty configuration | ported |
| `test_sprite_units.py::GeneratorHookTests.test_no_units_emits_no_hooks` | `generated/cases.ml` — hook order and empty configuration | ported |
| `test_sprite_units.py::GeneratorHookTests.test_undecoded_pc_is_an_error` | `selection/selection.ml` — required unit hooks and executable hits | covered-by |
| `test_sprite_units.py::GeneratorHookTests.test_slim_profile_must_retain_hooks` | `selection/selection.ml` — required unit hooks and executable hits | covered-by |

## Verification

The frozen Python suite collected 121 cases at base revision `c70c277`.
With the shared ROM directory and the pytest executable on the Nix shell's path,
`uv run --locked pytest tests/recomp` gave 120 passes and one failure in 161.73 s.
`test_decision_diff.py::test_decoder_diff_audits_missing_ordinary_rows`
expected the `cas2-extension` explanation for bytes `0e fc 00 00 05 04`
and got empty output. The failure was already present before the test changes.

The OCaml build and forced test run passed in 23.48 s wall time in the
requested Nix shell.

Landmakrj coverage reported `Callm 13`, `Rtm 11`, and `total 24`.

The persistence case links the real runtime profile session and supplies the
fixed CRC of its two-byte ROM fixture. Alcotest verifies the merged profile
through the OCaml profile loader. The generated ABI header is compiled against
the runtime header, which evaluates its ABI version guard.

Two existing issues surfaced during the audit. The Python indirect-chain
fixture writes `7629` at `0x602` over the extension word of the staged
`MOVEA.L` at `0x600` after it has already decoded the instruction. The OCaml
fixture keeps its ROM bytes and decoded operand consistent. The existing
enhancement name escaping check expects a surrogate pair for `😀`.
Compiling `constexpr auto name = "\ud83d\ude00";` as C++20 reproduces GCC's
invalid universal character error.
