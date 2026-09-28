cmake_minimum_required(VERSION 3.29)

if(NOT EXISTS "${DEMO}" OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "Set DEMO to demo_bench and OUTPUT_DIR to the capture directory")
endif()

function(capture output)
    execute_process(COMMAND "${DEMO}" ${ARGN} --static --smoke 3 --capture-dir "${output}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 60)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Static capture failed (${result}):\n${stdout}\n${stderr}")
    endif()
endfunction()

function(check_frames directory expected_count)
    file(GLOB frames "${directory}/*.bmp")
    list(LENGTH frames count)
    if(NOT count EQUAL expected_count)
        message(FATAL_ERROR "${directory}: expected ${expected_count} captures, found ${count}")
    endif()
    foreach(frame IN LISTS frames)
        string(REGEX REPLACE "_[0-9][0-9][0-9]\\.bmp$" "_000.bmp" first "${frame}")
        file(SHA256 "${first}" reference)
        file(SHA256 "${frame}" actual)
        if(NOT actual STREQUAL reference)
            message(FATAL_ERROR "Static frame changed: ${frame}")
        endif()
    endforeach()
endfunction()

# Separate runs ensure stale files cannot satisfy a failed/missing capture.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(root "${OUTPUT_DIR}/${run_id}")
foreach(run RANGE 1 2)
    capture("${root}/run-${run}/overview")
    check_frames("${root}/run-${run}/overview" 60)
    foreach(view RANGE 1 6)
        capture("${root}/run-${run}/cube-${view}" 7 --only-scene --static-view "${view}")
        check_frames("${root}/run-${run}/cube-${view}" 3)
    endforeach()
endforeach()
file(GLOB_RECURSE first_run RELATIVE "${root}/run-1" "${root}/run-1/*.bmp")
foreach(relative IN LISTS first_run)
    file(SHA256 "${root}/run-1/${relative}" reference)
    file(SHA256 "${root}/run-2/${relative}" actual)
    if(NOT actual STREQUAL reference)
        message(FATAL_ERROR "Capture changed across processes: ${relative}")
    endif()
endforeach()

foreach(invalid_args IN ITEMS "999;--only-scene" "7;--static-view;7"
                              "7;--static-view;1" "7;--static;--static-view;1")
    execute_process(COMMAND "${DEMO}" ${invalid_args}
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET TIMEOUT 10)
    if(NOT result STREQUAL "2")
        message(FATAL_ERROR "Invalid arguments did not return 2: ${invalid_args} (${result})")
    endif()
endforeach()
message(STATUS "Static gate passed: 20 scenes + 6 cube views, 3 frames, 2 runs (156 BMPs).")
message(STATUS "Visual review directory: ${root}/run-1")
file(WRITE "${root}/index.html" "<!doctype html><html lang='en'><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>f4 static visual checks</title><style>
body{margin:28px;background:#e9e4d8;color:#182c2b;font-family:Georgia,serif}
h1{font-size:34px}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px}
figure{margin:0;background:#fffdf5;padding:10px}img{width:100%;display:block}figcaption{padding-top:8px;font-family:monospace}
</style><h1>f4 static visual checks</h1>
<p>20 scenes and six cube directions. Three identical frames per fixture, repeated in two processes.
Click a capture for full resolution. Stability is not proof of correct rendering; inspect the labelled controls.</p><main>")
foreach(relative IN LISTS first_run)
    if(relative MATCHES "_000\\.bmp$")
        file(APPEND "${root}/index.html"
            "<figure><a href='run-1/${relative}'><img src='run-1/${relative}' alt='${relative}'></a><figcaption>${relative}</figcaption></figure>\n")
    endif()
endforeach()
file(APPEND "${root}/index.html" "</main></html>\n")
message(STATUS "Gallery: ${root}/index.html")
