import { defineConfig } from 'vitepress'
import { withMermaid } from 'vitepress-plugin-mermaid'

// Published at https://ansxor.github.io/f3-recomp/ (GitHub project page), so
// every asset URL must be rooted at /f3-recomp/.
export default withMermaid(
  defineConfig({
    title: 'f3-recomp',
    description:
      'Land Maker Japan static recompiler, SDL3 runtime and optional rollback netplay.',
    base: '/f3-recomp/',
    cleanUrls: true,
    lastUpdated: true,
    // README-style relative links into the repo (../../runtime/...) are
    // intentionally not site pages; they are written as absolute GitHub URLs.
    themeConfig: {
      nav: [
        { text: 'Guide', link: '/guide/', activeMatch: '/guide/' },
        { text: 'Reference', link: '/reference/cli', activeMatch: '/reference/' },
        { text: 'Developer', link: '/developer/', activeMatch: '/developer/' },
      ],
      sidebar: {
        '/guide/': [
          {
            text: 'Guide',
            items: [
              { text: 'Overview and scope', link: '/guide/' },
              { text: 'Getting started', link: '/guide/getting-started' },
              { text: 'Controls and options', link: '/guide/running' },
              { text: 'Video and presentation', link: '/guide/video' },
              { text: 'Sound', link: '/guide/sound' },
              { text: 'Online play', link: '/guide/netplay' },
              { text: 'Troubleshooting', link: '/guide/troubleshooting' },
            ],
          },
        ],
        '/reference/': [
          {
            text: 'Reference',
            items: [
              { text: 'Command-line reference', link: '/reference/cli' },
              { text: 'Build options', link: '/reference/build-options' },
              { text: 'Game configuration', link: '/reference/game-config' },
              { text: 'Generated files', link: '/reference/generated-files' },
              { text: 'Tools and scripts', link: '/reference/tools' },
            ],
          },
        ],
        '/developer/': [
          {
            text: 'Start here',
            items: [
              { text: 'Developer overview', link: '/developer/' },
              { text: 'Architecture', link: '/developer/architecture' },
              { text: 'Porting and game-specific code', link: '/developer/porting' },
              { text: 'Validation evidence', link: '/developer/evidence' },
              { text: 'Historical user-doc examples', link: '/developer/user-doc-evidence' },
              { text: 'Repository tour', link: '/developer/repository' },
              { text: 'Build pipeline', link: '/developer/build-pipeline' },
              { text: 'Glossary', link: '/developer/glossary' },
              { text: 'Contributing and docs', link: '/developer/contributing' },
            ],
          },
          {
            text: 'Recompiler',
            collapsed: true,
            items: [
              { text: 'Overview', link: '/developer/recompiler/' },
              { text: 'ROM loading and config', link: '/developer/recompiler/rom-and-config' },
              { text: 'Instruction discovery', link: '/developer/recompiler/discovery' },
              { text: 'Instruction emission', link: '/developer/recompiler/emission' },
              { text: 'Addressing modes', link: '/developer/recompiler/addressing-modes' },
              { text: 'Blocks, dispatch and sharding', link: '/developer/recompiler/blocks-and-dispatch' },
              { text: 'Flags and timing', link: '/developer/recompiler/flags-and-timing' },
              { text: 'Instruction reference', link: '/developer/recompiler/instruction-reference' },
              { text: 'Exceptions and hooks', link: '/developer/recompiler/exceptions-and-hooks' },
              { text: 'Extending the emitter', link: '/developer/recompiler/extending-the-emitter' },
              { text: 'Limits and known issues', link: '/developer/recompiler/limits-and-known-issues' },
              { text: 'Sound-CPU compiler', link: '/developer/recompiler/sound-compiler' },
            ],
          },
          {
            text: 'Runtime',
            collapsed: true,
            items: [
              { text: 'Overview', link: '/developer/runtime/' },
              { text: 'CPU ABI', link: '/developer/runtime/cpu-abi' },
              { text: 'Machine class', link: '/developer/runtime/machine' },
              { text: 'Memory map', link: '/developer/runtime/memory-map' },
              { text: 'Scheduling and interrupts', link: '/developer/runtime/scheduling' },
              { text: 'Input, coins and EEPROM', link: '/developer/runtime/input-and-eeprom' },
              { text: 'SDL3 frontend', link: '/developer/runtime/frontend' },
              { text: 'Interpreter fallback', link: '/developer/runtime/interpreter' },
              { text: 'Musashi and core state', link: '/developer/runtime/musashi' },
              { text: 'Support files', link: '/developer/runtime/support' },
              { text: 'Replay and device checks', link: '/developer/runtime/replay-and-check' },
            ],
          },
          {
            text: 'Video',
            collapsed: true,
            items: [
              { text: 'Overview', link: '/developer/runtime/video/' },
              { text: 'F3 video hardware', link: '/developer/runtime/video/hardware' },
              { text: 'FDP renderer', link: '/developer/runtime/video/fdp' },
              { text: 'FDP sprites', link: '/developer/runtime/video/fdp-sprites' },
              { text: 'FDP mixing', link: '/developer/runtime/video/fdp-mixing' },
              { text: 'Game-data renderer', link: '/developer/runtime/video/game-hle' },
              { text: 'Producer hooks and guards', link: '/developer/runtime/video/producers' },
              { text: 'Scene and coordinates', link: '/developer/runtime/video/scene' },
              { text: 'Playfield tiles', link: '/developer/runtime/video/tiles' },
              { text: 'Text layer', link: '/developer/runtime/video/text' },
              { text: 'Game-data sprites', link: '/developer/runtime/video/sprites' },
              { text: 'Line profiles', link: '/developer/runtime/video/lines' },
              { text: 'Compositor', link: '/developer/runtime/video/compositor' },
              { text: 'Presentation', link: '/developer/runtime/video/presentation' },
              { text: 'Compare mode', link: '/developer/runtime/video/compare-mode' },
              { text: 'Parity and limits', link: '/developer/runtime/video/parity' },
              { text: 'Extending the renderer', link: '/developer/runtime/video/extending' },
            ],
          },
          {
            text: 'Audio',
            collapsed: true,
            items: [
              { text: 'Overview', link: '/developer/runtime/audio/' },
              { text: 'Sound CPU', link: '/developer/runtime/audio/sound-cpu' },
              { text: 'Native sound driver', link: '/developer/runtime/audio/native-driver' },
              { text: 'Mailbox', link: '/developer/runtime/audio/mailbox' },
              { text: 'Device time and output', link: '/developer/runtime/audio/timing' },
              { text: 'ES5505 sample voices', link: '/developer/runtime/audio/es5505' },
              { text: 'ES5510 effects DSP', link: '/developer/runtime/audio/es5510' },
              { text: 'DUART and gain', link: '/developer/runtime/audio/duart-and-gain' },
              { text: 'Traces and command ownership', link: '/developer/runtime/audio/tracing' },
              { text: 'Sound command extraction', link: '/developer/runtime/audio/extraction' },
              { text: 'Sequences and voice allocation', link: '/developer/runtime/audio/sequences' },
            ],
          },
          {
            text: 'Netplay',
            collapsed: true,
            items: [
              { text: 'Overview', link: '/developer/netplay/' },
              { text: 'Snapshots', link: '/developer/netplay/snapshots' },
              { text: 'Determinism rules', link: '/developer/netplay/determinism' },
              { text: 'Rollback engine', link: '/developer/netplay/rollback' },
              { text: 'Transport', link: '/developer/netplay/transport' },
              { text: 'Wire protocol', link: '/developer/netplay/protocol' },
              { text: 'Relay server', link: '/developer/netplay/server' },
              { text: 'Build identity', link: '/developer/netplay/build-identity' },
              { text: 'Frontend integration', link: '/developer/netplay/frontend-integration' },
              { text: 'Netplay oracle', link: '/developer/netplay/oracle' },
              { text: 'Writing a client', link: '/developer/netplay/writing-a-client' },
              { text: 'Debugging', link: '/developer/netplay/debugging' },
              { text: 'Limits', link: '/developer/netplay/limits' },
            ],
          },
          {
            text: 'Verification',
            collapsed: true,
            items: [
              { text: 'Testing strategy', link: '/developer/testing/' },
              { text: 'Instruction differential tests', link: '/developer/testing/differential' },
              { text: 'Seeded gameplay', link: '/developer/testing/gameplay-regression' },
              { text: 'MAME captures', link: '/developer/testing/mame' },
              { text: 'Frame comparison', link: '/developer/testing/frame-compare' },
              { text: 'Audio comparison', link: '/developer/testing/audio-compare' },
              { text: 'Sound tools', link: '/developer/testing/sound-tools' },
              { text: 'Unit checks', link: '/developer/testing/unit-checks' },
              { text: 'Netplay oracle', link: '/developer/testing/netplay-oracle' },
            ],
          },
        ],
      },
      socialLinks: [{ icon: 'github', link: 'https://github.com/ansxor/f3-recomp' }],
      editLink: {
        pattern: 'https://github.com/ansxor/f3-recomp/edit/main/docs/site/:path',
        text: 'Edit this page on GitHub',
      },
      search: { provider: 'local' },
      outline: { level: [2, 3] },
      footer: {
        message: 'Requires your own legally obtained ROM set. No ROMs or game data are distributed.',
      },
    },
    mermaid: {
      securityLevel: 'strict',
      // Preserve readable labels. The theme provides horizontal scrolling
      // instead of shrinking large diagrams to the document column.
      flowchart: { useMaxWidth: false },
      sequence: { useMaxWidth: false },
      state: { useMaxWidth: false },
      class: { useMaxWidth: false },
    },
    mermaidPlugin: { class: 'mermaid' },
  }),
)
