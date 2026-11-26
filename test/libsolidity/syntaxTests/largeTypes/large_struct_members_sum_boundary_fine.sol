// The total size of P is 2**31 + (2**31 - 32) == 2**32 - 32.
struct P {
    uint[2**26] a;
    uint[2**26 - 1] b;
}
// The total size of M[2] is 2 * (2**31 - 32) == 2**32 - 64.
struct M {
    uint[2**26 - 1] a;
}
contract C {
    function f(P calldata, M[2] calldata) external pure {}
    function g(P memory, M[2] memory) public pure {}
    function h() public pure {
        P memory p;
        M[2] memory m;
        p; m;
    }
}
