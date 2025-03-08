64 cores, limited to 12 waking
the key difference appears to be whether both producers are in the same CCD (cores 0-3)
even indexes 0 & 4 causes 10x slowdown
possible cause - sharing between producers in diff CCXs slows down production,
causing many more suspends of consumers. when consumers suspend, they move.
Q: test count how many times each consumer actually suspends vs. data is ready
A: They suspend never on fast run, and nearly every time on slow run...
   So perhaps the issue isn't the moving, but the suspending every time.
   Or perhaps the issue isn't the suspending at all - but the producers conflicting.
Q: 2 producers on diff CCX's and one consumer
A: It's possible to cause a small number of suspensions this way, but only about 1%.
   This about 2x's the runtime but doesn't 10x... so the majority of the issue seems to be the suspending.
Q: How many times do the consumers actually move?
- Count the number of migrations by checking thread_index after resuming

fix: how to prevent consumers from moving when suspending?

Slow:
push tids: 500000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
push tids: 0 0 0 0 0 0 0 0 500000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9249 12811 17162 3466 2447 2398 2545 0 9015 12661 17792 3005 2327 2253 2530 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9108 12765 17436 3304 2632 2380 2560 0 8990 12736 17830 3046 2496 2262 2512 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9357 12982 17193 3307 2461 2365 2534 0 8913 12647 18223 2994 2321 2321 2516 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9184 12748 17293 3330 2519 2350 2479 0 9126 12692 17998 3001 2294 2230 2535 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9125 12860 17246 3460 2388 2372 2541 0 8915 12789 17940 3057 2298 2228 2569 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9219 12713 17451 3391 2524 2339 2542 0 8971 12910 17943 3015 2247 2225 2501 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9248 12977 17283 3354 2461 2370 2566 0 9013 12907 18036 2961 2363 2259 2556 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9121 12959 17231 3362 2471 2387 2561 0 9058 12835 17981 3029 2283 2291 2546 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9197 12956 17082 3394 2485 2426 2583 0 8937 12946 17806 2978 2353 2176 2565 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 9350 12899 17335 3490 2548 2384 2652 0 9039 12804 17696 2995 2343 2207 2505 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
2 prod  10 cons  650708 us

Fast:
push tids: 0 0 0 500000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
push tids: 500000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 84294 82 0 62 41 37 47 1 2 4 6 6 1 2 6 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 84 85313 0 180 38 41 46 5 5 7 6 4 1 1 4 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 80 91 0 90142 34 38 40 5 5 5 14 7 0 3 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 74 94 1 52 35 38 90126 4 6 3 6 5 5 4 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 68 81 0 54 46 90236 53 9 3 4 8 5 3 1 6 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 68 118 1 43 89974 44 38 3 4 4 10 6 7 4 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 75 79 0 114 39 51 48 2 4 4 3 5 4 3 119585 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 91 133 0 51 35 33 37 7 6 10 2 4 113656 3 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 119 90 1 54 30 31 42 6 6 6 5 3 6 119286 3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
pull tids: 0 75 157 1 47 37 30 65 7 4 4 6 113645 3 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 
2 prod  10 cons  42165 us