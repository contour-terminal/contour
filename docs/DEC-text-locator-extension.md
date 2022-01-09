# DEC Locators

Documented in DEC STD 070 manuall, section 13 (Text Locator Extension).

`vttest` (`11.8.5.8`) provides tests.

Helpful resource: [](https://vt100.net/shuford/terminal/dec_vt_mouse.html)


## Table of Contents

- Select Locator Events (DECSLE)
- Enable Locator Reporting (DECELR)
- Enable Filter Rectangle (DECEFR)
- Request Locator Position (DECRQLP)
- DEC Locator Report (DECLRP)

### Enable Locator Reporting (DECELR).

```
CSI Ps ; Pu ' z
          Enable Locator Reporting (DECELR).
          Valid values for the first parameter:
            Ps = 0  -> Locator disabled (default).
            Ps = 1  -> Locator enabled.
            Ps = 2  -> Locator enabled for one report, then disabled.
          The second parameter specifies the coordinate unit for locator
          reports.
          Valid values for the second parameter:
            Pu = 0  or omitted -> default to character cells.
            Pu = 1  <- device physical pixels.
            Pu = 2  <- character cells.

```

### Request Locator Position (DECRQLP).

```
CSI Ps ' |
          Request Locator Position (DECRQLP).
          Valid values for the parameter are:
            Ps = 0 , 1 or omitted -> transmit a single DECLRP locator
          report.

          If Locator Reporting has been enabled by a DECELR, xterm will
          respond with a DECLRP Locator Report.  This report is also
          generated on button up and down events if they have been
          enabled with a DECSLE, or when the locator is detected outside
          of a filter rectangle, if filter rectangles have been enabled
          with a DECEFR.

            <- CSI Pe ; Pb ; Pr ; Pc ; Pp &  w


          Parameters are [event;button;row;column;page].
          Valid values for the event:
            Pe = 0  <- locator unavailable - no other parameters sent.
            Pe = 1  <- request - xterm received a DECRQLP.
            Pe = 2  <- left button down.
            Pe = 3  <- left button up.
            Pe = 4  <- middle button down.
            Pe = 5  <- middle button up.
            Pe = 6  <- right button down.
            Pe = 7  <- right button up.
            Pe = 8  <- M4 button down.
            Pe = 9  <- M4 button up.
            Pe = 1 0  <- locator outside filter rectangle.
          The "button" parameter is a bitmask indicating which buttons
          are pressed:
            Pb = 0  <- no buttons down.
            Pb & 1  <- right button down.
            Pb & 2  <- middle button down.
            Pb & 4  <- left button down.
            Pb & 8  <- M4 button down.
          The "row" and "column" parameters are the coordinates of the
          locator position in the xterm window, encoded as ASCII
          decimal.
          The "page" parameter is not used by xterm.
```

### Select Locator Events (DECSLE)

```
CSI Pm ' {
          Select Locator Events (DECSLE).
          Valid values for the first (and any additional parameters)
          are:
            Ps = 0  -> only respond to explicit host requests (DECRQLP).
          This is default.  It also cancels any filter rectangle.
            Ps = 1  -> report button down transitions.
            Ps = 2  -> do not report button down transitions.
            Ps = 3  -> report button up transitions.
            Ps = 4  -> do not report button up transitions.

```

### Enable Filter Rectangle (DECEFR)

```
CSI Pt ; Pl ; Pb ; Pr ' w
          Enable Filter Rectangle (DECEFR), VT420 and up.
          Parameters are [top;left;bottom;right].
          Defines the coordinates of a filter rectangle and activates
          it.  Anytime the locator is detected outside of the filter
          rectangle, an outside rectangle event is generated and the
          rectangle is disabled.  Filter rectangles are always treated
          as "one-shot" events.  Any parameters that are omitted default
          to the current locator position.  If all parameters are
          omitted, any locator motion will be reported.  DECELR always
          cancels any previous rectangle definition.

```

### DEC Locator Report (DECLRP)

This is the reply to `DECRQLP`.




