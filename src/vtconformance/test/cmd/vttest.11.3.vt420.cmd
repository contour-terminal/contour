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
Read: 3 
Note: choice 11.3: Test of VT420 features
Text: Menu 11.3: 
Text: VT420 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT320 features
Text:           2. Test cursor-movement
Text:           3. Test editing sequences
Text:           4. Test keyboard-control
Text:           5* Test macro-definition (DECDMAC)
Text:           6. Test rectangular area functions
Text:           7. Test reporting functions
Text:           8. Test screen-display functions
Text: 
Text:           Enter choice number (0 - 8): 
Read: * 
Note: Selecting all choices
Note: choice 11.3.1: Test VT320 features
Text: Menu 11.3.1: 
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
Note: choice 11.3.2: Test cursor-movement
Text: Menu 11.3.2: 
Text: VT420 Cursor-Movement Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT320 features
Text:           2. Enable DECOM (origin mode)
Text:           3. Enable DECLRMM (left/right mode)
Text:           4. Top/Bottom margins are reset
Text:           5. Left/right margins are reset
Text:           6. Do not color test-regions (xterm)
Text:           7. Test Back Index (DECBI)
Text:           8. Test Forward Index (DECFI)
Text:           9. Test cursor movement within margins
Text:           10. Test other movement (CR/HT/LF/FF) within margins
Text: 
Text:           Enter choice number (0 - 10): 
Read: 0 
Note: choice 11.3.3: Test editing sequences
Text: Menu 11.3.3: 
Text: VT420 Editing Sequence Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Enable DECOM (origin mode)
Text:           2. Enable DECLRMM (left/right mode)
Text:           3. Top/Bottom margins are reset
Text:           4. Left/right margins are reset
Text:           5. Do not color test-regions (xterm)
Text:           6. Show DECRQM response for DECLRMM
Text:           7. Show DECRQSS response for DECSTBM
Text:           8. Show DECRQSS response for DECSLRM
Text:           9. Test insert/delete column (DECIC, DECDC)
Text:           10. Test vertical scrolling (IND, RI)
Text:           11. Test insert/delete line (IL, DL)
Text:           12. Test insert/delete char (ICH, DCH)
Text:           13. Test ASCII formatting (BS, CR, TAB)
Text: 
Text:           Enter choice number (0 - 13): 
Read: 0 
Note: choice 11.3.4: Test keyboard-control
Text: Menu 11.3.4: 
Text: VT420 Keyboard-Control Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test Backarrow key (DECBKM)
Text:           2. Test Numeric keypad (DECNKM)
Text:           3. Test Keyboard usage (DECKBUM)
Text:           4. Test Key position (DECKPM)
Text:           5* Test Enable Local Functions (DECELF)
Text:           6* Test Local Function-Key Control (DECLFKC)
Text:           7* Test Select Modifier-Key Reporting (DECSMKR)
Text: 
Text:           Enter choice number (0 - 7): 
Read: 0 
Note: choice 11.3.5: Test macro-definition (DECDMAC)
Text: Sorry, test not implemented:

  Test macro-definition (DECDMAC)Data: P u s h <32> < R E T U R N > 
Read: 
Note: choice 11.3.6: Test rectangular area functions
Text: Menu 11.3.6: 
Text: VT420 Rectangular Area Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Enable DECOM (origin mode)
Text:           2. Enable DECLRMM (left/right mode)
Text:           3. Top/Bottom margins are reset
Text:           4. Left/right margins are reset
Text:           5. Do not color test-regions (xterm)
Text:           6. Do not use line-drawing characters
Text:           7. Test Change-Attributes in Rectangular Area (DECCARA)
Text:           8. Test Copy Rectangular area (DECCRA)
Text:           9. Test Erase Rectangular area (DECERA)
Text:           10. Test Fill Rectangular area (DECFRA)
Text:           11. Test Reverse-Attributes in Rectangular Area (DECRARA)
Text:           12. Test Selective-Erase Rectangular area (DECSERA)
Text: 
Text:           Enter choice number (0 - 12): 
Read: 0 
Note: choice 11.3.7: Test reporting functions
Text: Menu 11.3.7: 
Text: VT420 Reports
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT320 features
Text:           2. Test Presentation State Reports
Text:           3. Test Device Status Reports (DSR)
Text: 
Text:           Enter choice number (0 - 3): 
Read: 0 
Note: choice 11.3.8: Test screen-display functions
Text: Menu 11.3.8: 
Text: VT420 Screen-Display Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT320 features
Text:           2. Test Select Number of Lines per Screen (DECSNLS)
Text: 
Text:           Enter choice number (0 - 2): 
Read: 0 
Text: Menu 11.3: 
Text: VT420 Tests
Text: Choose test type:
Text: 
Text:           0. Exit
Text:           1. Test VT320 features
Text:           2. Test cursor-movement
Text:           3. Test editing sequences
Text:           4. Test keyboard-control
Text:           5* Test macro-definition (DECDMAC)
Text:           6. Test rectangular area functions
Text:           7. Test reporting functions
Text:           8. Test screen-display functions
Text: 
Text:           Enter choice number (0 - 8): 
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


