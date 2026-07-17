Note: Setup Terminal with test-defaults
Note: UTF-8 is not enabled
Send: <27> [ 0 c 
Wait: 1
Read: <27> [ ? 6 5 ; 1 ; 4 ; 7 ; 9 ; 1 8 ; 2 1 ; 2 2 ; 2 9 ; 5 2 ; 3 1 4 c 
Done: 1
Send: <27> P $ q " p <27> \ 
Wait: 2
Read: <27> P 1 $ r 6 5 ; 1 " p <27> \ 
Done: 2
Send: <27> [ > c 
Wait: 3
Read: <27> [ > 6 5 ; 7 0 0 ; 0 c 
Done: 3
Send: <27> [ 1 ; 7 ; 0 , | 
Note: Max Operating Level: 5
Note: Cur Operating Level: 5
Note: Derived terminal-id: 525
Note: set_level(5)
Note: ...set_level(5) in=7, out=7, fsm=7
Send: <27> [ ? 1 l 
Send: <27> [ ? 3 l 
Send: <27> [ ? 4 l 
Send: <27> [ ? 5 l 
Send: <27> [ ? 6 l 
Send: <27> [ ? 7 h 
Send: <27> [ ? 8 l 
Send: <27> [ ? 4 0 h 
Send: <27> [ ? 4 5 l 
Send: <27> [ r 
Send: <27> [ 0 m 
Text: VT100 test program, version 2.7
Text:  (20251205)
Text: Line speed 38400bd 
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test of cursor movements
Text:           2. Test of screen features
Text:           3. Test of character sets
Text:           4. Test of double-sized characters
Text:           5. Test of keyboard
Text:           6. Test of terminal reports
Text:           7. Test of VT52 mode
Text:           8. Test of VT102 features (Insert/Delete Char/Line)
Text:           9. Test of known bugs
Text:           10. Test of reset and self-test
Text:           11. Test non-VT100 (e.g., VT220, XTERM) terminals
Text:           12. Modify test-parameters
Text: 
Text:           Enter choice number (0 - 12): 
Read: 1 1 
Note: choice 11: Test non-VT100 (e.g., VT220, XTERM) terminals
Text: Menu 11: 
Text: Non-VT100 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test of VT220 features
Text:           2. Test of VT320 features
Text:           3. Test of VT420 features
Text:           4. Test of VT520 features
Text:           5. Test ISO-6429 cursor-movement
Text:           6. Test ISO-6429 colors
Text:           7. Test other ISO-6429 features
Text:           8. Test XTERM special features
Text: 
Text:           Enter choice number (0 - 8): 
Read: 2 
Note: choice 11.2: Test of VT320 features
Text: Menu 11.2: 
Text: VT320 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT220 features
Text:           2. Test cursor-movement
Text:           3. Test page-format controls
Text:           4. Test page-movement controls
Text:           5. Test reporting functions
Text:           6. Test screen-display functions
Text: 
Text:           Enter choice number (0 - 6): 
Read: * 
Note: Selecting all choices
Note: choice 11.2.1: Test VT220 features
Text: Menu 11.2.1: 
Text: VT220 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test reporting functions
Text:           2. Test screen-display functions
Text:           3. Test 8-bit controls (S7C1T/S8C1T)
Text:           4. Test Printer (MC)
Text:           5. Test Soft Character Sets (DECDLD)
Text:           6. Test Soft Terminal Reset (DECSTR)
Text:           7. Test User-Defined Keys (DECUDK)
Text: 
Text:           Enter choice number (0 - 7): 
Read: 0 
Note: choice 11.2.2: Test cursor-movement
Text: Menu 11.2.2: 
Text: VT320 Cursor-Movement Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test Pan down (SU)
Text:           2. Test Pan up (SD)
Text:           3* Test Vertical Cursor Coupling (DECVCCM)
Text:           4* Test Page Cursor Coupling (DECPCCM)
Text: 
Text:           Enter choice number (0 - 4): 
Read: 0 
Note: choice 11.2.3: Test page-format controls
Text: Menu 11.2.3: 
Text: Page Format Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test set columns per page (DECSCPP)
Text:           2. Test set lines per page (DECSLPP)
Text: 
Text:           Enter choice number (0 - 2): 
Read: 0 
Note: choice 11.2.4: Test page-movement controls
Text: Menu 11.2.4: 
Text: Page Format Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1* Test Next Page (NP)
Text:           2* Test Preceding Page (PP)
Text:           3* Test Page Position Absolute (PPA)
Text:           4* Test Page Position Backward (PPB)
Text:           5* Test Page Position Relative (PPR)
Text: 
Text:           Enter choice number (0 - 5): 
Read: 0 
Note: choice 11.2.5: Test reporting functions
Text: Menu 11.2.5: 
Text: VT320 Reports
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT220 features
Text:           2. Test Device Status Report (DSR)
Text:           3. Test Presentation State Reports
Text:           4. Test Terminal State Reports
Text:           5. Test User-Preferred Supplemental Set
Text:           6. Test Window Report (DECRPDE)
Text: 
Text:           Enter choice number (0 - 6): 
Read: 0 
Note: choice 11.2.6: Test screen-display functions
Text: Menu 11.2.6: 
Text: VT320 Screen-Display Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT220 features
Text:           2. Test Status line (DECSASD/DECSSDT)
Text: 
Text:           Enter choice number (0 - 2): 
Read: 0 
Text: Menu 11.2: 
Text: VT320 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT220 features
Text:           2. Test cursor-movement
Text:           3. Test page-format controls
Text:           4. Test page-movement controls
Text:           5. Test reporting functions
Text:           6. Test screen-display functions
Text: 
Text:           Enter choice number (0 - 6): 
Read: 0 
Text: Menu 11: 
Text: Non-VT100 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test of VT220 features
Text:           2. Test of VT320 features
Text:           3. Test of VT420 features
Text:           4. Test of VT520 features
Text:           5. Test ISO-6429 cursor-movement
Text:           6. Test ISO-6429 colors
Text:           7. Test other ISO-6429 features
Text:           8. Test XTERM special features
Text: 
Text:           Enter choice number (0 - 8): 
Read: 0 
Text: VT100 test program, version 2.7
Text:  (20251205)
Text: Line speed 38400bd 
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test of cursor movements
Text:           2. Test of screen features
Text:           3. Test of character sets
Text:           4. Test of double-sized characters
Text:           5. Test of keyboard
Text:           6. Test of terminal reports
Text:           7. Test of VT52 mode
Text:           8. Test of VT102 features (Insert/Delete Char/Line)
Text:           9. Test of known bugs
Text:           10. Test of reset and self-test
Text:           11. Test non-VT100 (e.g., VT220, XTERM) terminals
Text:           12. Modify test-parameters
Text: 
Text:           Enter choice number (0 - 12): 
Read: 0 
Note: Cleanup & exit
Note: set_level(5)
Note: ...set_level(5) in=7, out=7, fsm=7
Send: <27> [ ? 1 l 
Send: <27> [ ? 3 l 
Send: <27> [ ? 5 l 
Send: <27> [ ? 6 l 
Send: <27> [ ? 7 h 
Send: <27> [ ? 8 h 
Send: <27> [ r 
Send: <27> [ 0 m 
Text: That's all, folks!
Text: 


