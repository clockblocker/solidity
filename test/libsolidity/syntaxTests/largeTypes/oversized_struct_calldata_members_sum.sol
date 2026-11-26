// The total size of P is 2**31 + 2**31 == 2**32.
struct P {
    uint[2**26] a;
    uint[2**26] b;
}
contract C {
    function f(P calldata) external pure {}
}
// ----
// TypeError 1534: (129-139): Type too large for calldata.
