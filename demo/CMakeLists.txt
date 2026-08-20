function(demo name) 
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE AstraLib)
endfunction()

demo(atomicRingBufferDemo)
demo(timerDemo)