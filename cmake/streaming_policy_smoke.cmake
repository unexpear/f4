cmake_minimum_required(VERSION 3.29)
if(NOT EXISTS "${DEMO}" OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "Set DEMO to f4_streaming_demo and OUTPUT_DIR to the capture parent")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(root "${OUTPUT_DIR}/${run_id}")
file(MAKE_DIRECTORY "${root}")
set(checkpoints voxel_home voxel_returned voxel_reverse voxel_empty hex_home hex_returned hex_under
    hex_camera_home hex_camera_away hex_camera_returned
    region_height region_voxel_0 region_hex_0 region_restored_0
    region_voxel_1 region_hex_1 region_restored_1 region_voxel_2 region_hex_2 region_restored_2
    mixed_home mixed_reverse mixed_overhead mixed_boundary mixed_restored
    meters_home meters_overhead meters_boundary
    owned_home owned_overhead owned_boundary
    capped_home capped_east capped_corner
    voxel_cut_open voxel_cut_east voxel_cut_oblique
    corner_before corner_after corner_close corner_east corner_north
    corner_motion_0 corner_motion_1 corner_motion_2 corner_motion_3 corner_restored
    corner_live_home corner_live_pending corner_live_staging corner_live_edited corner_live_restored
    corner_departure_pending corner_away_plain corner_return_pending corner_returned)
foreach(run RANGE 1 2)
    execute_process(COMMAND "${DEMO}" --verify-policies --output "${root}/run-${run}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 150)
    file(WRITE "${root}/run-${run}.log" "${stdout}\n${stderr}")
    if(NOT result STREQUAL "0" OR NOT stdout MATCHES "PASS: policy diagnostics")
        message(FATAL_ERROR "Policy diagnostics failed (${result}):\n${stdout}\n${stderr}")
    endif()
    file(GLOB images "${root}/run-${run}/*.bmp")
    file(GLOB sweep_frames "${root}/run-${run}/sweep_tick_*.bmp")
    list(LENGTH images count)
    list(LENGTH sweep_frames sweep_count)
    math(EXPR expected_images "112 + ${sweep_count}")
    if(NOT count EQUAL expected_images)
        message(FATAL_ERROR "Expected 112 policy captures plus ${sweep_count} sweep presents; found ${count}")
    endif()
    foreach(checkpoint IN LISTS checkpoints)
        file(SHA256 "${root}/run-${run}/${checkpoint}.bmp" original)
        file(SHA256 "${root}/run-${run}/${checkpoint}_repeat.bmp" repeated)
        if(NOT original STREQUAL repeated)
            message(FATAL_ERROR "Policy checkpoint blinks: ${checkpoint}")
        endif()
    endforeach()
    file(SHA256 "${root}/run-${run}/mixed_home_meshes.csv" home_meshes)
    file(SHA256 "${root}/run-${run}/mixed_restored_meshes.csv" restored_meshes)
    if(NOT home_meshes STREQUAL restored_meshes)
        message(FATAL_ERROR "Mixed-policy return changed CPU meshes, including hidden geometry")
    endif()
endforeach()
foreach(checkpoint IN LISTS checkpoints)
    file(SHA256 "${root}/run-1/${checkpoint}.bmp" original)
    file(SHA256 "${root}/run-2/${checkpoint}.bmp" repeated)
    if(NOT original STREQUAL repeated)
        message(FATAL_ERROR "Policy checkpoint differs across processes: ${checkpoint}")
    endif()
    file(SHA256 "${root}/run-1/${checkpoint}_meshes.csv" mesh_original)
    file(SHA256 "${root}/run-2/${checkpoint}_meshes.csv" mesh_repeated)
    if(NOT mesh_original STREQUAL mesh_repeated)
        message(FATAL_ERROR "CPU mesh checkpoint differs across processes: ${checkpoint}")
    endif()
endforeach()
file(SHA256 "${root}/run-1/mixed_boundary_audit.csv" audit_original)
file(SHA256 "${root}/run-2/mixed_boundary_audit.csv" audit_repeated)
if(NOT audit_original STREQUAL audit_repeated)
    message(FATAL_ERROR "Mixed boundary audit differs across processes")
endif()
file(SHA256 "${root}/run-1/meter_surface_probes.csv" probes_original)
file(SHA256 "${root}/run-2/meter_surface_probes.csv" probes_repeated)
if(NOT probes_original STREQUAL probes_repeated)
    message(FATAL_ERROR "Published triangle surface probes differ across processes")
endif()
file(SHA256 "${root}/run-1/owned_surface_probes.csv" owned_original)
file(SHA256 "${root}/run-2/owned_surface_probes.csv" owned_repeated)
if(NOT owned_original STREQUAL owned_repeated)
    message(FATAL_ERROR "Regional surface probes differ across processes")
endif()
foreach(evidence capped_surface_probes capped_wall_probes voxel_cut_probes corner_display_queries corner_reuse corner_motion_residency)
    file(SHA256 "${root}/run-1/${evidence}.csv" original)
    file(SHA256 "${root}/run-2/${evidence}.csv" repeated)
    if(NOT original STREQUAL repeated)
        message(FATAL_ERROR "Hex cut wall evidence differs across processes: ${evidence}")
    endif()
endforeach()
file(GLOB sweep_run_1 "${root}/run-1/sweep_tick_*.bmp")
file(GLOB sweep_run_2 "${root}/run-2/sweep_tick_*.bmp")
list(SORT sweep_run_1)
list(SORT sweep_run_2)
list(LENGTH sweep_run_1 sweep_count_1)
list(LENGTH sweep_run_2 sweep_count_2)
if(NOT sweep_count_1 EQUAL sweep_count_2 OR sweep_count_1 LESS 17)
    message(FATAL_ERROR "Sweep presents differ across processes: ${sweep_count_1} versus ${sweep_count_2}")
endif()
math(EXPR sweep_last "${sweep_count_1} - 1")
foreach(index RANGE ${sweep_last})
    list(GET sweep_run_1 ${index} sweep_a)
    list(GET sweep_run_2 ${index} sweep_b)
    get_filename_component(sweep_name_a "${sweep_a}" NAME)
    get_filename_component(sweep_name_b "${sweep_b}" NAME)
    if(NOT sweep_name_a STREQUAL sweep_name_b)
        message(FATAL_ERROR "Sweep present names differ: ${sweep_name_a} versus ${sweep_name_b}")
    endif()
    file(SHA256 "${sweep_a}" sweep_hash_a)
    file(SHA256 "${sweep_b}" sweep_hash_b)
    if(NOT sweep_hash_a STREQUAL sweep_hash_b)
        message(FATAL_ERROR "Sweep present differs across processes: ${sweep_name_a}")
    endif()
endforeach()
foreach(evidence height_hex_profiles.csv height_hex_profile_summary.csv height_hex_profiles.svg
    height_hex_wall_triangles.csv height_hex_wall_summary.csv height_hex_walls.svg height_hex_trim.csv height_hex_stitch.csv
    height_hex_replacement.csv height_hex_composition.csv height_hex_junction.csv)
    file(SHA256 "${root}/run-1/${evidence}" original)
    file(SHA256 "${root}/run-2/${evidence}" repeated)
    if(NOT original STREQUAL repeated)
        message(FATAL_ERROR "Height/hex boundary profile differs across processes: ${evidence}")
    endif()
endforeach()
execute_process(COMMAND "${DEMO}" --verify-policies --load "${root}/missing.f4sd" --output "${root}/invalid-run"
    RESULT_VARIABLE invalid OUTPUT_QUIET ERROR_VARIABLE diagnostic TIMEOUT 10)
if(NOT invalid STREQUAL "1" OR NOT diagnostic MATCHES "Policy diagnostics do not use the height-only save format"
   OR EXISTS "${root}/invalid-run")
    message(FATAL_ERROR "Policy diagnostics must reject height-save loading before initialization")
endif()
file(WRITE "${root}/index.html" "<!doctype html><html lang='en'><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>M16 policy diagnostics</title>
<style>body{margin:28px;background:#e9e4d8;color:#182c2b;font-family:Georgia,serif}
main{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,400px),1fr));gap:18px}
figure{margin:0;padding:12px;background:#fffdf5}img{width:100%}figcaption{font-family:monospace}</style>
<h1>M16 policy diagnostics</h1><p>Fixed-anchor inspection plus a camera-driven tiled-hex route. Nine voxel/hex chunks,
unload/return, empty voxel retirement, three stationary height/voxel/hex region cycles.
Tiled hex layout removes the old large packing holes; numerical boundary closure is audited below. Mixed-policy regional seams remain OPEN.
Concurrent fixture: seven height, two voxel, two hex chunks; four inspection angles and 90 unsynchronized region/camera mutations.
The <a href='run-1/mixed_boundary_audit.csv'>triangle-centroid audit</a> detects encroachment, not every gap or intersection.
Meter-footprint comparison: +/-96m center selection with 14 height, four voxel, two hex chunks. The formerly excluded voxel half-region is now resident.
This fixed comparison has sufficient capacity; cross-policy transition seams are still OPEN.
<a href='run-1/meter_surface_probes.csv'>Published triangle surface probes</a> include all three policies and observations near the unresolved boundary.
The opt-in regional-surface comparison adds two intersecting hex chunks and clips published CPU triangles to their policy regions.
Its <a href='run-1/owned_surface_probes.csv'>64 hex surface probes</a> must hit; cut walls and height-transition geometry are NOT implemented.
The separate hex-cut-wall mode closes prism cuts to their existing floor, not to neighboring terrain heights.
Below-surface views and <a href='run-1/capped_wall_probes.csv'>32 outward wall probes</a> inspect the new triangles.
The cell-masked voxel-cut comparison exposes solid faces without capping its tunnel; <a href='run-1/voxel_cut_probes.csv'>90 face/tunnel probes</a> check both.
The <a href='run-1/height_hex_profiles.svg'>height/hex boundary charts</a> measure both published top profiles, not a generated bridge.
<a href='run-1/height_hex_profiles.csv'>Exact breakpoints and triangle provenance</a>,
<a href='run-1/height_hex_profile_summary.csv'>gap/overlap lengths</a> and
<a href='run-1/height_hex_profile_sources.csv'>local mesh-version stamps</a> expose the remaining transition inputs.
The disk-save demo remains height-only. All 224 checkpoint captures repeat exactly within/across two processes.
Each corner-sweep world-update tick also presents one <code>sweep_tick_*.bmp</code>. Those presents are hashed across processes. They use the view already installed for that tick and are not a hard realtime deadline.
CPU mesh digests match across processes and on mixed home/return.</p>
<p>The <a href='run-1/height_hex_walls.svg'>isolated connecting-wall diagram</a> draws generated triangles,
with <a href='run-1/height_hex_wall_triangles.csv'>float geometry and span ownership</a> and
<a href='run-1/height_hex_wall_summary.csv'>area and culled ray evidence</a>.
These walls are NOT published over existing skirts/caps. Replacement and world integration remain OPEN.</p>
<p><a href='run-1/height_hex_trim.csv'>Private-copy coplanar trim evidence</a> measures removal of old boundary faces
without changing top profiles. These source copies are NOT published; coordinated replacement remains OPEN.</p>
<p><a href='run-1/height_hex_stitch.csv'>Private-copy top-edge stitching</a> checks exact full-edge matches
between generated wall points and subdivided source triangles. This is CPU geometry evidence, not world publication.</p>
<p><a href='run-1/height_hex_replacement.csv'>Immutable common-frame replacement</a> packages source meshes and walls,
retaining original triangle provenance and dependency stamps. Mesh hashes and combined wall queries repeat exactly.
The package is not installed into the renderer/world; multi-boundary publication remains OPEN.</p>
<p><a href='run-1/height_hex_composition.csv'>Disjoint shared-source composition</a> applies two boundary strips
to one source table, retaining earlier edge cuts and emitting each source once. Original-source wall rays and
complete top-edge matches are checked after both operations. The strips stop 16 m before their corner;
this fixture does not exercise corner closure. This CPU-only composition is NOT rendered or published.</p>
<p>The <a href='run-1/height_hex_junction.csv'>complete shared-corner composition</a> extends both strips to 64 m.
It checks all wall rays and source top-edge matches plus an exactly paired, oppositely oriented vertical corner edge.
Endpoint sector heights must agree; inconsistent heights across absent arms and nonmanifold vertical edges are rejected.
This is CPU corner geometry acceptance, NOT coordinated rendering/query publication.</p>
<p><a href='run-1/corner_after.bmp'>Displayed corner replacement</a> now stages composed meshes by policy,
suppresses the represented originals, and switches view queries to that same immutable geometry.
<a href='run-1/corner_before.bmp'>Before</a>, <a href='run-1/corner_close.bmp'>close-up</a>, four camera positions,
and exact original-image restoration are checked. <a href='run-1/corner_display_queries.csv'>30 wall probes</a>
query the installed view, not newer CPU world meshes. This explicit inspection mode freezes resident synchronization;
automatic live rebuild/publication and general collision ownership remain OPEN.</p>
<p>The explicit live-request controller now rebuilds composed geometry on a worker and stages a complete
resident/render/query replacement. <a href='run-1/corner_live_pending.bmp'>Pending source edits</a> retain the old view;
<a href='run-1/corner_live_staging.bmp'>partial GPU staging</a> also leaves that view unchanged. Upload and retirement
counts are now admitted per tick; the final visibility/query commit is still indivisible on the main thread.
<a href='run-1/corner_live_edited.bmp'>accepted edits</a> change both geometry and displayed-surface queries.
<a href='run-1/corner_live_restored.bmp'>Restoration</a> reproduces the original composed image exactly.
<a href='run-1/corner_reuse.csv'>Reuse counters</a> require every unchanged resident to keep its existing GPU mesh;
only changed residents and composed policy groups are uploaded. Borrowed originals stay owned by the accepted view until commit.
This fixed-boundary fixture does not prove automatic boundary discovery, arbitrary moving-residency coverage or hard budgets.</p>
<p>Region-map discovery in caller-selected windows now selects departure and return targets.
Plain retirement stages resident terrain before removing the corner. The
<a href='run-1/corner_departure_pending.bmp'>pending departure</a> retains the old corner;
<a href='run-1/corner_away_plain.bmp'>away terrain</a> becomes the displayed/query view only after commit.
<a href='run-1/corner_return_pending.bmp'>Return staging</a> retains that view until the home corner is ready,
then <a href='run-1/corner_returned.bmp'>home restoration</a> is exact. Windows are caller-selected and sources settle
at route endpoints; this is not residency-derived window selection. One presented frame is hashed for each world-update tick of that sweep.</p>
<figure><img src='run-1/height_hex_profiles.svg' alt='Measured height and hex boundary profiles; transitions remain open'></figure>
<figure><img src='run-1/height_hex_walls.svg' alt='Isolated connecting wall triangles, not published terrain'></figure><main>")
foreach(checkpoint IN LISTS checkpoints)
    file(APPEND "${root}/index.html" "<figure><a href='run-1/${checkpoint}.bmp'><img src='run-1/${checkpoint}.bmp' alt='${checkpoint}'></a><figcaption>${checkpoint}</figcaption></figure>\n")
endforeach()
file(GLOB sweep_gallery "${root}/run-1/sweep_tick_*.bmp")
list(SORT sweep_gallery)
foreach(frame IN LISTS sweep_gallery)
    get_filename_component(sweep_leaf "${frame}" NAME)
    file(APPEND "${root}/index.html" "<figure><a href='run-1/${sweep_leaf}'><img src='run-1/${sweep_leaf}' alt='${sweep_leaf}'></a><figcaption>${sweep_leaf}</figcaption></figure>\n")
endforeach()
file(APPEND "${root}/index.html" "</main></html>")
message(STATUS "Policy diagnostics passed: 224 repeatable captures; corner retirement/retargeting, incremental upload/edit/restoration and GPU lifecycle. General transitions remain OPEN.")
message(STATUS "Gallery: ${root}/index.html")
