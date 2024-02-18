# Save and Restore SGR attributes.

## Feature detection

Use `SGRSAVE` (`CSI # {`) save the currently set SGR attributes,
and `SGRRESTORE` (`CSI # }`) to restore the previously saved SGR attributes.

## Relation to xterm's XTPUSHSGR / XTPOPSGR

Both, `XTPUSHSGR` and `XTPOPSGR` are in its most basic form equivalent to `SGRSAVE` and `SGRRESTORE`,
but the xterm extenions push and pop using a stack rather than save and restore using a simple 
storage location, and the xterm equivalent does allow pushing/popping only certain SGR attributes.

This is needless functionality that should not be implemented by a terminal but rather in the applications itself.
