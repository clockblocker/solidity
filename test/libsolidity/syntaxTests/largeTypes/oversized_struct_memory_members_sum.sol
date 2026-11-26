// The total size of P is 2**31 + 2**31 == 2**32.
struct P {
    uint[2**26] a;
    uint[2**26] b;
}
contract C {
    function f() public pure {
        P memory p;
        p;
    }
}
// ----
// TypeError 1534: (153-163): Type too large for memory.
