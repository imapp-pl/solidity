/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Translates Yul code from EVM dialect to eWasm dialect.
 */

#include <libyul/backends/wasm/EVMToEWasmTranslator.h>

#include <libyul/backends/wasm/WordSizeTransform.h>
#include <libyul/backends/wasm/WasmDialect.h>
#include <libyul/optimiser/ExpressionSplitter.h>
#include <libyul/optimiser/FunctionGrouper.h>
#include <libyul/optimiser/MainFunction.h>
#include <libyul/optimiser/FunctionHoister.h>
#include <libyul/optimiser/Disambiguator.h>
#include <libyul/optimiser/NameDisplacer.h>
#include <libyul/optimiser/OptimiserStep.h>

#include <libyul/AsmParser.h>
#include <libyul/AsmAnalysis.h>
#include <libyul/AsmAnalysisInfo.h>
#include <libyul/Object.h>

#include <liblangutil/ErrorReporter.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/SourceReferenceFormatter.h>

using namespace std;
using namespace dev;
using namespace yul;
using namespace langutil;

namespace
{
static string const polyfill{R"({
function or_bool(a, b, c, d) -> r {
	r := i64.or(i64.or(a, b), i64.or(c, d))
}
// returns a + y + c plus carry value on 64 bit values.
// c should be at most 1
function add_carry(x, y, c) -> r, r_c {
	let t := i64.add(x, y)
	r := i64.add(t, c)
	r_c := i64.or(
		i64.lt_u(t, x),
		i64.lt_u(r, t)
	)
}
function add(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	let carry
	r4, carry := add_carry(x4, y4, 0)
	r3, carry := add_carry(x3, y3, carry)
	r2, carry := add_carry(x2, y2, carry)
	r1, carry := add_carry(x1, y1, carry)
}
function bit_negate(x) -> y {
	y := i64.xor(x, 0xffffffffffffffff)
}
function sub(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	// x - y = x + (~y + 1)
	let carry
	r4, carry := add_carry(x4, bit_negate(y4), 1)
	r3, carry := add_carry(x3, bit_negate(y3), carry)
	r2, carry := add_carry(x2, bit_negate(y2), carry)
	r1, carry := add_carry(x1, bit_negate(y1), carry)
}
function sub320(x1, x2, x3, x4, x5, y1, y2, y3, y4, y5) -> r1, r2, r3, r4, r5 {
	// x - y = x + (~y + 1)
	let carry
	r5, carry := add_carry(x4, bit_negate(y5), 1)
	r4, carry := add_carry(x4, bit_negate(y4), carry)
	r3, carry := add_carry(x3, bit_negate(y3), carry)
	r2, carry := add_carry(x2, bit_negate(y2), carry)
	r1, carry := add_carry(x1, bit_negate(y1), carry)
}
function sub512(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8) -> r1, r2, r3, r4, r5, r6, r7, r8 {
	// x - y = x + (~y + 1)
	let carry
	r8, carry := add_carry(x4, bit_negate(y8), 1)
	r7, carry := add_carry(x3, bit_negate(y7), carry)
	r6, carry := add_carry(x3, bit_negate(y6), carry)
	r5, carry := add_carry(x3, bit_negate(y5), carry)
	r4, carry := add_carry(x3, bit_negate(y4), carry)
	r3, carry := add_carry(x3, bit_negate(y3), carry)
	r2, carry := add_carry(x2, bit_negate(y2), carry)
	r1, carry := add_carry(x1, bit_negate(y1), carry)
}
function split(x) -> hi, lo {
	hi := i64.shr_u(x, 32)
	lo := i64.and(x, 0xffffffff)
}
// Multiplies two 64 bit values resulting in a 128 bit
// value split into two 64 bit values.
function mul_64x64_128(x, y) -> hi, lo {
	let xh, xl := split(x)
	let yh, yl := split(y)

	let t0 := i64.mul(xl, yl)
	let t1 := i64.mul(xh, yl)
	let t2 := i64.mul(xl, yh)
	let t3 := i64.mul(xh, yh)

	let t0h, t0l := split(t0)
	let u1 := i64.add(t1, t0h)
	let u1h, u1l := split(u1)
	let u2 := i64.add(t2, u1l)

	lo := i64.or(i64.shl(u2, 32), t0l)
	hi := i64.add(t3, i64.add(i64.shr_u(u2, 32), u1h))
}
// Multiplies two 128 bit values resulting in a 256 bit
// value split into four 64 bit values.
function mul_128x128_256(x1, x2, y1, y2) -> r1, r2, r3, r4 {
	let ah, al := mul_64x64_128(x1, y1)
	let     bh, bl := mul_64x64_128(x1, y2)
	let     ch, cl := mul_64x64_128(x2, y1)
	let         dh, dl := mul_64x64_128(x2, y2)

	r4 := dl

	let carry1, carry2
	let t1, t2

	r3, carry1 := add_carry(bl, cl, 0)
	r3, carry2 := add_carry(r3, dh, 0)

	t1, carry1 := add_carry(bh, ch, carry1)
	r2, carry2 := add_carry(t1, al, carry2)

	r1 := i64.add(i64.add(ah, carry1), carry2)
}
// Multiplies two 256 bit values resulting in a 512 bit
// value split into eight 64 bit values.
function mul_256x256_512(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4, r5, r6, r7, r8 {
	let a1, a2, a3, a4 := mul_128x128_256(x1, x2, y1, y2)
	let         b1, b2, b3, b4 := mul_128x128_256(x1, x2, y3, y4)
	let         c1, c2, c3, c4 := mul_128x128_256(x3, x4, y1, y2)
	let                 d1, d2, d3, d4 := mul_128x128_256(x3, x4, y3, y4)

	r8 := d4
	r7 := d3

	let carry1, carry2
	let t1, t2

	r6, carry1 := add_carry(b4, c4, 0)
	r6, carry2 := add_carry(r6, d2, 0)

	r5, carry1 := add_carry(b3, c3, carry1)
	r5, carry2 := add_carry(r5, d1, carry2)

	r4, carry1 := add_carry(a4, b2, carry1)
	r4, carry2 := add_carry(r4, c2, carry2)

	r3, carry1 := add_carry(a3, b1, carry1)
	r3, carry2 := add_carry(r3, c1, carry2)

	r2, carry1 := add_carry(a2, carry1, carry1)
	r1 := i64.add(a1, carry1)
}
function mul(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	// TODO it would actually suffice to have mul_128x128_128 for the first two.
	let b1, b2, b3, b4 := mul_128x128_256(x3, x4, y1, y2)
	let c1, c2, c3, c4 := mul_128x128_256(x1, x2, y3, y4)
	let d1, d2, d3, d4 := mul_128x128_256(x3, x4, y3, y4)
	r4 := d4
	r3 := d3
	let t1, t2
	t1, t2, r1, r2 := add(0, 0, b3, b4, 0, 0, c3, c4)
	t1, t2, r1, r2 := add(0, 0, r1, r2, 0, 0, d1, d2)
}
function div(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	// Implementation was taken from https://github.com/ewasm/evm2wasm/blob/master/wasm/DIV.wast
	if iszero(y1, y2, y3, y4) {
		invalid()
	}
	let m1 := 0
	let m2 := 0
	let m3 := 0
	let m4 := 1

	for {} i64.and(i64.eqz(i64.clz(y1)), lt_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4)) {} {
		y1, y2, y3, y4 := shl(y1, y2, y3, y4, 0, 0, 0, 1)
		m1, m2, m3, m4 := shl(m1, m2, m3, m4, 0, 0, 0, 1)
	}

	for {} i64.xor(iszero(m1, m2, m3, m4), 1) {} {
		if lt_256x256_64(y1, y2, y3, y4, x1, x2, x3, x4) {
			x1, x2, x3, x4 := sub(x1, x2, x3, x4, y1, y2, y3, y4)
			r1, r2, r3, r4 := add(r1, r2, r3, r4, m1, m2, m3, m4)
		}

		y1, y2, y3, y4 := shr(y1, y2, y3, y4, 0, 0, 0, 1)
		m1, m2, m3, m4 := shr(m1, m2, m3, m4, 0, 0, 0, 1)
	}
}
function mod(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	if iszero(y1, y2, y3, y4) {
		invalid()
	}

	let m1 := 0
	let m2 := 0
	let m3 := 0
	let m4 := 1

	for {} 1 {} {
		if i64.or(i64.eqz(i64.clz(y1)), gte_256x256_64(y1, y2, y3, y4, x1, x2, x3, x4)) {
			break
		}

		y1 := i64.add(i64.shl(y1, 1), i64.shr_u(y2, 63))
		y2 := i64.add(i64.shl(y2, 1), i64.shr_u(y3, 63))
		y3 := i64.add(i64.shl(y3, 1), i64.shr_u(y4, 63))
		y4 := i64.shl(y4, 1)

		m1 := i64.add(i64.shl(m1, 1), i64.shr_u(m2, 63))
		m2 := i64.add(i64.shl(m2, 1), i64.shr_u(m3, 63))
		m3 := i64.add(i64.shl(m3, 1), i64.shr_u(m4, 63))
		m4 := i64.shl(m4, 1)
	}

	for {} i64.xor(iszero(m1, m2, m3, m4), 1) {} {
		if gte_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4) {
			x1, x2, x3, x4 := sub(x1, x2, x3, x4, y1, y2, y3, y4)
		}

		// y = y >> 1
		y1 := i64.add(i64.shr_u(y1, 1), i64.shl(y2, 63))
		y2 := i64.add(i64.shr_u(y2, 1), i64.shl(y3, 63))
		y3 := i64.add(i64.shr_u(y3, 1), i64.shl(y4, 63))
		y4 := i64.shr_u(y4, 1)

		// m = m >> 1
		m1 := i64.add(i64.shr_u(m1, 1), i64.shl(m2, 63))
		m2 := i64.add(i64.shr_u(m2, 1), i64.shl(m3, 63))
		m3 := i64.add(i64.shr_u(m3, 1), i64.shl(m4, 63))
		m4 := i64.shr_u(m4, 1)
	}
}
function mod320(x1, x2, x3, x4, x5, y1, y2, y3, y4, y5) -> r1, r2, r3, r4, r5 {
	if iszero320(y1, y2, y3, y4, y5) {
		invalid()
	}

	let m1 := 0
	let m2 := 0
	let m3 := 0
	let m4 := 0
	let m5 := 1

	for {} 1 {} {
		if i64.or(i64.eqz(i64.clz(y1)), gte_320x320_64(y1, y2, y3, y4, y5, x1, x2, x3, x4, x5)) {
			break
		}
		// y = y << 1
		y1 := i64.add(i64.shl(y1, 1), i64.shr_u(y2, 63))
		y2 := i64.add(i64.shl(y2, 1), i64.shr_u(y3, 63))
		y3 := i64.add(i64.shl(y3, 1), i64.shr_u(y4, 63))
		y4 := i64.add(i64.shl(y4, 1), i64.shr_u(y5, 63))
		y5 := i64.shl(y5, 1)

		// m = m << 1
		m1 := i64.add(i64.shl(m1, 1), i64.shr_u(m2, 63))
		m2 := i64.add(i64.shl(m2, 1), i64.shr_u(m3, 63))
		m3 := i64.add(i64.shl(m3, 1), i64.shr_u(m4, 63))
		m4 := i64.add(i64.shl(m4, 1), i64.shr_u(m5, 63))
		m5 := i64.shl(m5, 1)
	}

	for {} i64.xor(iszero320(m1, m2, m3, m4, y5), 1) {} {
		if gte_320x320_64(x1, x2, x3, x4, x5, y1, y2, y3, y4, y5) {
			x1, x2, x3, x4, x5 := sub320(x1, x2, x3, x4, x5, y1, y2, y3, y4, y4)
		}

		// y = y >> 1
		y1 := i64.add(i64.shr_u(y1, 1), i64.shl(y2, 63))
		y2 := i64.add(i64.shr_u(y2, 1), i64.shl(y3, 63))
		y3 := i64.add(i64.shr_u(y3, 1), i64.shl(y4, 63))
		y4 := i64.add(i64.shr_u(y4, 1), i64.shl(y5, 63))
		y5 := i64.shr_u(y5, 1)

		// m = m >> 1
		m1 := i64.add(i64.shr_u(m1, 1), i64.shl(m2, 63))
		m2 := i64.add(i64.shr_u(m2, 1), i64.shl(m3, 63))
		m3 := i64.add(i64.shr_u(m3, 1), i64.shl(m4, 63))
		m4 := i64.add(i64.shr_u(m4, 1), i64.shl(m5, 63))
		m5 := i64.shr_u(m5, 1)
	}
}
function mod512(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8) -> r1, r2, r3, r4, r5, r6, r7, r8 {
	// TODO: implement
	if iszero512(y1, y2, y3, y4, y5, y6, y7, y8) {
		invalid()
	}

	let m1 := 0
	let m2 := 0
	let m3 := 0
	let m4 := 0
	let m5 := 0
	let m6 := 0
	let m7 := 0
	let m8 := 1

	for {} 1 {} {
		if i64.or(i64.eqz(i64.clz(y1)), gte_512x512_64(y1, y2, y3, y4, y5, y6, y7, y8, x1, x2, x3, x4, x5, x6, x7, x8)) {
			break
		}
		// y = y << 1
		y1 := i64.add(i64.shl(y1, 1), i64.shr_u(y2, 63))
		y2 := i64.add(i64.shl(y2, 1), i64.shr_u(y3, 63))
		y3 := i64.add(i64.shl(y3, 1), i64.shr_u(y4, 63))
		y4 := i64.add(i64.shl(y4, 1), i64.shr_u(y5, 63))
		y5 := i64.add(i64.shl(y5, 1), i64.shr_u(y6, 63))
		y6 := i64.add(i64.shl(y6, 1), i64.shr_u(y7, 63))
		y7 := i64.add(i64.shl(y7, 1), i64.shr_u(y8, 63))
		y8 := i64.shl(y8, 1)

		// m = m << 1
		m1 := i64.add(i64.shl(m1, 1), i64.shr_u(m2, 63))
		m2 := i64.add(i64.shl(m2, 1), i64.shr_u(m3, 63))
		m3 := i64.add(i64.shl(m3, 1), i64.shr_u(m4, 63))
		m4 := i64.add(i64.shl(m4, 1), i64.shr_u(m5, 63))
		m2 := i64.add(i64.shl(m5, 1), i64.shr_u(m6, 63))
		m3 := i64.add(i64.shl(m6, 1), i64.shr_u(m7, 63))
		m4 := i64.add(i64.shl(m7, 1), i64.shr_u(m8, 63))
		m8 := i64.shl(m8, 1)
	}

	for {} i64.xor(iszero512(m1, m2, m3, m4, m5, m6, m7, m8), 1) {} {
		if gte_512x512_64(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8) {
			x1, x2, x3, x4, x5, x6, x7, x8 := sub512(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8)
		}

		// y = y >> 1
		y1 := i64.add(i64.shr_u(y1, 1), i64.shl(y2, 63))
		y2 := i64.add(i64.shr_u(y2, 1), i64.shl(y3, 63))
		y3 := i64.add(i64.shr_u(y3, 1), i64.shl(y4, 63))
		y4 := i64.add(i64.shr_u(y4, 1), i64.shl(y5, 63))
		y2 := i64.add(i64.shr_u(y2, 1), i64.shl(y3, 63))
		y3 := i64.add(i64.shr_u(y3, 1), i64.shl(y4, 63))
		y4 := i64.add(i64.shr_u(y4, 1), i64.shl(y5, 63))
		y1 := i64.shr_u(y5, 1)

		// m = m >> 1
		m1 := i64.add(i64.shr_u(m1, 1), i64.shl(m2, 63))
		m2 := i64.add(i64.shr_u(m2, 1), i64.shl(m3, 63))
		m3 := i64.add(i64.shr_u(m3, 1), i64.shl(m4, 63))
		m4 := i64.add(i64.shr_u(m4, 1), i64.shl(m5, 63))
		m5 := i64.add(i64.shr_u(m5, 1), i64.shl(m6, 63))
		m6 := i64.add(i64.shr_u(m6, 1), i64.shl(m7, 63))
		m7 := i64.add(i64.shr_u(m7, 1), i64.shl(m8, 63))
		m8 := i64.shr_u(m8, 1)
	}
}
function smod(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	let m1 := 0
	let m2 := 0
	let m3 := 0
	let m4 := 1

	let sign := i64.shr_u(x4, 63)

	if i64.eqz(i64.clz(x1)) {
		x1, x2, x3, x4 := xor(x1, x2, x3, x4, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff)
		x1, x2, x3, x4 := add(x1, x2, x3, x4, 0, 0, 0, 1)
	}

	if i64.eqz(i64.clz(y1)) {
		y1, y2, y3, y4 := xor(y1, y2, y3, y4, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff)
		y1, y2, y3, y4 := add(y1, y2, y3, y4, 0, 0, 0, 1)
	}

	if iszero(y1, y2, y3, y4) {
		invalid()
	}
	if i64.xor(iszero(y1, y2, y3, y4), 1) {
		for {} i64.and(i64.xor(i64.eqz(i64.clz(y1)), 1), gte_256x256_64(y1, y2, y3, y4, x1, x2, x3, x4)) {} {
			y1, y2, y3, y4 := shl(y1, y2, y3, y4, 0, 0, 0, 1)
			m1, m2, m3, m4 := shl(m1, m2, m3, m4, 0, 0, 0, 1)
		}

		for {} i64.xor(iszero(m1, m2, m3, m4), 1) {} {
			if gte_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4) {
				x1, x2, x3, x4 := sub(x1, x2, x3, x4, y1, y2, y3, y4)
			}
			y1, y2, y3, y4 := shl(y1, y2, y3, y4, 0, 0, 0, 1)
			m1, m2, m3, m4 := shl(m1, m2, m3, m4, 0, 0, 0, 1)
		}
	}
}
function exp(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	r4 := 1
	for {} i64.xor(iszero(y1, y2, y3, y4), 1) {} {
		if i64.and(y4, 1) {
			r1, r2, r3, r4 := mul(r1, r2, r3, r4, x1, x2, x3, x4)
		}
		x1, x2, x3, x4 := mul(x1, x2, x3, x4, x1, x2, x3, x4)
		y1, y2, y3, y4 := shr(y1, y2, y3, y4, 0, 0, 0, 1)
	}
}

function byte(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	if i64.eqz(i64.or(i64.or(x1, x2), x3)) {
		let component
		switch i64.div_u(x4, 8)
		case 0 { component := y1 }
		case 1 { component := y2 }
		case 2 { component := y3 }
		case 3 { component := y4 }
		x4 := i64.mul(i64.rem_u(x4, 8), 8)
		r4 := i64.shr_u(component, i64.sub(56, x4))
		r4 := i64.and(0xff, r4)
	}
}
function xor(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	r1 := i64.xor(x1, y1)
	r2 := i64.xor(x2, y2)
	r3 := i64.xor(x3, y3)
	r4 := i64.xor(x4, y4)
}
function or(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	r1 := i64.or(x1, y1)
	r2 := i64.or(x2, y2)
	r3 := i64.or(x3, y3)
	r4 := i64.or(x4, y4)
}
function and(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	r1 := i64.and(x1, y1)
	r2 := i64.and(x2, y2)
	r3 := i64.and(x3, y3)
	r4 := i64.and(x4, y4)
}
function not(x1, x2, x3, x4) -> r1, r2, r3, r4 {
	let mask := 0xffffffffffffffff
	r1, r2, r3, r4 := xor(x1, x2, x3, x4, mask, mask, mask, mask)
}
function iszero(x1, x2, x3, x4) -> r {
	r := i64.eqz(i64.or(i64.or(x1, x2), i64.or(x3, x4)))
}
function iszero320(x1, x2, x3, x4, x5) -> r {
	r := i64.eqz(i64.or(i64.or(i64.or(x1, x2), i64.or(x3, x4)), x5))
}
function iszero512(x1, x2, x3, x4, x5, x6, x7, x8) -> r {
	r := i64.eqz(i64.or(i64.or(i64.or(i64.or(i64.or(i64.or(x1, x2), i64.or(x3, x4)), x5), x6), x7), x8))
}
function eq(x1, x2, x3, x4, y1, y2, y3, y4) -> r1, r2, r3, r4 {
	if i64.eq(x1, y1) {
		if i64.eq(x2, y2) {
			if i64.eq(x3, y3) {
				if i64.eq(x4, y4) {
					r4 := 1
				}
			}
		}
	}
}

// returns 0 if a == b, -1 if a < b and 1 if a > b
function cmp(a, b) -> r {
	switch i64.lt_u(a, b)
	case 1 { r := 0xffffffffffffffff }
	default {
		r := i64.ne(a, b)
	}
}
function lt_320x320_64(x1, x2, x3, x4, x5, y1, y2, y3, y4, y5) -> z {
	switch cmp(x1, y1)
	case 0 {
		switch cmp(x2, y2)
		case 0 {
			switch cmp(x3, y3)
			case 0 {
				switch cmp(x4, y4)
				case 0 {
					z := i64.lt_u(x5, y5)
				}
				case 1 { z := 0 }
				default { z := 1 }
			}
			case 1 { z := 0 }
			default { z := 1 }
		}
		case 1 { z := 0 }
		default { z := 1 }
	}
	case 1 { z := 0 }
	default { z := 1 }
}
function lt_512x512_64(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8) -> z {
	switch cmp(x1, y1)
	case 0 {
		switch cmp(x2, y2)
		case 0 {
			switch cmp(x3, y3)
			case 0 {
				switch cmp(x4, y4)
				case 0 {
					switch cmp(x5, y5)
					case 0 {
						switch cmp(x6, y6)
						case 0 {
							switch cmp(x7, y7)
							case 0 {
								z := i64.lt_u(x8, y8)
							}
							case 1 { z := 0 }
							default { z := 1 }
						}
						case 1 { z := 0 }
						default { z := 1 }
					}
					case 1 { z := 0 }
					default { z := 1 }
				}
				case 1 { z := 0 }
				default { z := 1 }
			}
			case 1 { z := 0 }
			default { z := 1 }
		}
		case 1 { z := 0 }
		default { z := 1 }
	}
	case 1 { z := 0 }
	default { z := 1 }
}
function lt_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4) -> z {
	switch cmp(x1, y1)
	case 0 {
		switch cmp(x2, y2)
		case 0 {
			switch cmp(x3, y3)
			case 0 {
				z := i64.lt_u(x4, y4)
			}
			case 1 { z := 0 }
			default { z := 1 }
		}
		case 1 { z := 0 }
		default { z := 1 }
	}
	case 1 { z := 0 }
	default { z := 1 }
}
function lt(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	z4 := lt_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4)
}
function gte_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4) -> z {
	z := i64.xor(lt_256x256_64(x1, x2, x3, x4, y1, y2, y3, y4), 1)
}
function gte_320x320_64(x1, x2, x3, x4, x5, y1, y2, y3, y4, y5) -> z {
	z := i64.xor(lt_320x320_64(x1, x2, x3, x4, x5, y1, y2, y3, y4, y5), 1)
}
function gte_512x512_64(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8) -> z {
	z := i64.xor(lt_512x512_64(x1, x2, x3, x4, x5, x6, x7, x8, y1, y2, y3, y4, y5, y6, y7, y8), 1)
}
function slt(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	// TODO correct?
	x1 := i64.add(x1, 0x8000000000000000)
	y1 := i64.add(y1, 0x8000000000000000)
	z1, z2, z3, z4 := lt(x1, x2, x3, x4, y1, y2, y3, y4)
}
function sgt(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	z1, z2, z3, z4 := slt(y1, y2, y3, y4, x1, x2, x3, x4)
}

function shl_single(a, amount) -> x, y {
	// amount < 64
	x := i64.shr_u(a, i64.sub(64, amount))
	y := i64.shl(a, amount)
}

function shl(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	if i64.and(i64.eqz(x1), i64.eqz(x2)) {
		if i64.eqz(x3) {
			if i64.lt_u(x4, 256) {
				if i64.ge_u(x4, 128) {
					y1 := y3
					y2 := y4
					y3 := 0
					y4 := 0
					x4 := i64.sub(x4, 128)
				}
				if i64.ge_u(x4, 64) {
					y1 := y2
					y2 := y3
					y3 := y4
					y4 := 0
					x4 := i64.sub(x4, 64)
				}
				let t, r
				t, z4 := shl_single(y4, x4)
				r, z3 := shl_single(y3, x4)
				z3 := i64.or(z3, t)
				t, z2 := shl_single(y2, x4)
				z2 := i64.or(z2, r)
				r, z1 := shl_single(y1, x4)
				z1 := i64.or(z1, t)
			}
		}
	}
}

function shr_single(a, amount) -> x, y {
	// amount < 64
	y := i64.shl(a, i64.sub(64, amount))
	x := i64.shr_u(a, amount)
}

function shr(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	if i64.and(i64.eqz(x1), i64.eqz(x2)) {
		if i64.eqz(x3) {
			if i64.lt_u(x4, 256) {
				if i64.ge_u(x4, 128) {
					y4 := y2
					y3 := y1
					y2 := 0
					y1 := 0
					x4 := i64.sub(x4, 128)
				}
				if i64.ge_u(x4, 64) {
					y4 := y3
					y3 := y2
					y2 := y1
					y1 := 0
					x4 := i64.sub(x4, 64)
				}
				let t
				z4, t := shr_single(y4, x4)
				z3, t := shr_single(y3, x4)
				z4 := i64.or(z4, t)
				z2, t := shr_single(y2, x4)
				z3 := i64.or(z3, t)
				z1, t := shr_single(y1, x4)
				z2 := i64.or(z2, t)
			}
		}
	}
}
function sar(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	let lz := i64.and(i64.shl(y1, 63), 1)
	if i64.eqz(i64.and(lz, 0xffffffffffffffff)) {
		z1, z2, z3, z4 := shr(x1, x2, x3, x4, y1, y2, y3, y4)
	}
	if i64.and(lz, 0xffffffffffffffff) {
		if i64.ge_u(x4, 256) {
			z1 := 0xffffffffffffffff
			z2 := 0xffffffffffffffff
			z3 := 0xffffffffffffffff
			z4 := 0xffffffffffffffff
		}
		if i64.lt_u(x4, 256) {
			let sr1, sr2, sr3, sr4 := shr(x1, x2, x3, x4, y1, y2, y3, y4)
			let mo1, mo2, mo3, mo4 := sub(0xffffffffffffffff, 0xffffffffffffffff, 0xffffffffffffffff, 0xffffffffffffffff, x1, x2, x3, x4)
			let sl1, sl2, sl3, sl4 := shl(mo1, mo2, mo3, mo4, y1, y2, y3, y4)
			z1, z2, z3, z4 := or(sr1, sr2, sr3, sr4,sl1, sl2, sl3, sl4)
		}
	}
}
function addmod(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	let carry
	let r1
	let r2
	let r3
	let r4
	r4, carry := add_carry(x4, y4, 0)
	r3, carry := add_carry(x3, y3, carry)
	r2, carry := add_carry(x2, y2, carry)
	r1, carry := add_carry(x1, y1, carry)

	let z5
	z1, z2, z3, z4, z5 := mod320(carry, r1, r2, r3, r4, 0, y1, y2, y3, y4)
}
function mulmod(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	let r1, r2, r3, r4, r5, r6, r7, r8 := mul_256x256_512(x1, x2, x3, x4, y1, y2, y3, y4)
	let t1
	let t2
	let t3
	let t4
	t1, t2, t3, t4, z1, z2, z3, z4 := mod512(r1, r2, r3, r4, r5, r6, r7, r8, 0, 0, 0, 0, y1, y2, y3, y4)
}
function signextend(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	if i64.lt_u(y4, 31) {
		let signBit := i64.add(i64.mul(i64.shr_u(i64.shl(y4, 32), 32), 8), 7)
		let sm1, sm2, sm3, sm4 := shl(0, 0, 0, 1, 0, 0, 0, signBit)
		let vm1, vm2, vm3, vm4 := sub(sm1, sm2, sm3, sm4, 0, 0, 0, 1)
		let ys1, ys2, ys3, ys4 := and(y1, y2, y3, y4, sm1, sm2, sm3, sm4)
		let isZero := iszero(ys1, ys2, ys3, ys4)

		if isZero {
			z1, z2, z3, z4 := and(y1, y2, y3, y4, vm1, vm2, vm3, vm4)
		}
		if i64.eqz(isZero) {
			let nvm1, nvm2, nvm3, nvm4 := not(vm1, vm2, vm3, vm4)
			z1, z2, z3, z4 := and(y1, y2, y3, y4, nvm1, nvm2, nvm3, nvm4)
		}
	}
}
function u256_to_i128(x1, x2, x3, x4) -> v1, v2 {
	if i64.ne(0, i64.or(x1, x2)) { invalid() }
	v2 := x4
	v1 := x3
}

function u256_to_i64(x1, x2, x3, x4) -> v {
	if i64.ne(0, i64.or(i64.or(x1, x2), x3)) { invalid() }
	v := x4
}

function u256_to_i32(x1, x2, x3, x4) -> v {
	if i64.ne(0, i64.or(i64.or(x1, x2), x3)) { invalid() }
	if i64.ne(0, i64.shr_u(x4, 32)) { invalid() }
	v := x4
}

function u256_to_byte(x1, x2, x3, x4) -> v {
	if i64.ne(0, i64.or(i64.or(x1, x2), x3)) { invalid() }
	if i64.ne(0, i64.shr_u(x4, 56)) { invalid() }
	v := x4
}

function u256_to_i32ptr(x1, x2, x3, x4) -> v {
	v := u256_to_i32(x1, x2, x3, x4)
}

function keccak256(x1, x2, x3, x4, y1, y2, y3, y4) -> z1, z2, z3, z4 {
	// TODO implement
	unreachable()
}

function address() -> z1, z2, z3, z4 {
	eth.getAddress(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function balance(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	mstore_internal(0, x1, x2, x3, x4)
	eth.getExternalBalance(0, 32)
	z1, z2, z3, z4 := mload_internal(32)
}
function origin() -> z1, z2, z3, z4 {
	eth.getTxOrigin(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function caller() -> z1, z2, z3, z4 {
	eth.getCaller(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function callvalue() -> z1, z2, z3, z4 {
	eth.getCallValue(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function calldataload(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	z1, z2, z3, z4 := calldatasize()
	eth.callDataCopy(i64.add(u256_to_i32ptr(x1, x2, x3, x4), 64), 0, u256_to_i32(z1, z2, z3, z4))
}
function calldatasize() -> z1, z2, z3, z4 {
	z4 := eth.getCallDataSize()
}
function calldatacopy(x1, x2, x3, x4, y1, y2, y3, y4, z1, z2, z3, z4) {
	eth.callDataCopy(
		// scratch - TODO: overflow check
		i64.add(u256_to_i32ptr(x1, x2, x3, x4), 64),
		u256_to_i32(y1, y2, y3, y4),
		u256_to_i32(z1, z2, z3, z4)
	)
}

// Needed?
function codesize() -> z1, z2, z3, z4 {
	eth.getCodeSize(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function codecopy(x1, x2, x3, x4, y1, y2, y3, y4, z1, z2, z3, z4) {
	eth.codeCopy(
		// scratch - TODO: overflow check
		i64.add(u256_to_i32ptr(x1, x2, x3, x4), 64),
		u256_to_i32(y1, y2, y3, y4),
		u256_to_i32(z1, z2, z3, z4)
	)
}
function datacopy(x1, x2, x3, x4, y1, y2, y3, y4, z1, z2, z3, z4) {
	// TODO correct?
	codecopy(x1, x2, x3, x4, y1, y2, y3, y4, z1, z2, z3, z4)
}

function gasprice() -> z1, z2, z3, z4 {
	eth.getTxGasPrice(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function extcodesize(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	mstore_internal(0, x1, x2, x3, x4)
	z4 := eth.getExternalCodeSize(0)
}
function extcodehash(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	// TODO implement. It'a keccak256 of extcode. Is will be implemented in ewasm later.
	unreachable()
}
function extcodecopy(a1, a2, a3, a4, p1, p2, p3, p4, o1, o2, o3, o4, l1, l2, l3, l4) {
	let ecs1, ecs2, ecs3, ecs4 := extcodesize(a1, a2, a3, a4)
	let codeSize := u256_to_i32(ecs1, ecs2, ecs3, ecs4)
	mstore_internal(0, a1, a2, a3, a4)  // TODO: Is it already there after extcodesize call?
	let codeOffset := u256_to_i32(o1, o2, o3, o4)
	let codeLength := u256_to_i32(l1, l2, l3, l4)
	eth.externalCodeCopy(0, i64.add(u256_to_i32ptr(p1, p2, p3, p4), 64), codeOffset, codeLength)
}

function returndatasize() -> z1, z2, z3, z4 {
	z4 := eth.getReturnDataSize()
}
function returndatacopy(x1, x2, x3, x4, y1, y2, y3, y4, z1, z2, z3, z4) {
	eth.returnDataCopy(
		// scratch - TODO: overflow check
		i64.add(u256_to_i32ptr(x1, x2, x3, x4), 64),
		u256_to_i32(y1, y2, y3, y4),
		u256_to_i32(z1, z2, z3, z4)
	)
}

function blockhash(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	let r := eth.getBlockHash(u256_to_i64(x1, x2, x3, x4), 0)
	if i64.eqz(r) {
		z1, z2, z3, z4 := mload_internal(0)
	}
}
function coinbase() -> z1, z2, z3, z4 {
	eth.getBlockCoinbase(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function timestamp() -> z1, z2, z3, z4 {
	z4 := eth.getBlockTimestamp()
}
function number() -> z1, z2, z3, z4 {
	z4 := eth.getBlockNumber()
}
function difficulty() -> z1, z2, z3, z4 {
	eth.getBlockDifficulty(0)
	z1, z2, z3, z4 := mload_internal(0)
}
function gaslimit() -> z1, z2, z3, z4 {
	z4 := eth.getBlockGasLimit()
}

function pop(x1, x2, x3, x4) {
}


function endian_swap_16(x) -> y {
	let hi := i64.and(i64.shl(x, 8), 0xff00)
	let lo := i64.and(i64.shr_u(x, 8), 0xff)
	y := i64.or(hi, lo)
}

function endian_swap_32(x) -> y {
	let hi := i64.shl(endian_swap_16(x), 16)
	let lo := endian_swap_16(i64.shr_u(x, 16))
	y := i64.or(hi, lo)
}

function endian_swap(x) -> y {
	let hi := i64.shl(endian_swap_32(x), 32)
	let lo := endian_swap_32(i64.shr_u(x, 32))
	y := i64.or(hi, lo)
}
function save_temp_mem_32() -> t1, t2, t3, t4 {
	t1 := i64.load(0)
	t2 := i64.load(8)
	t3 := i64.load(16)
	t4 := i64.load(24)
}
function restore_temp_mem_32(t1, t2, t3, t4) {
	i64.store(0, t1)
	i64.store(8, t2)
	i64.store(16, t3)
	i64.store(24, t4)
}
function save_temp_mem_64() -> t1, t2, t3, t4, t5, t6, t7, t8 {
	t1 := i64.load(0)
	t2 := i64.load(8)
	t3 := i64.load(16)
	t4 := i64.load(24)
	t5 := i64.load(32)
	t6 := i64.load(40)
	t7 := i64.load(48)
	t8 := i64.load(54)
}
function restore_temp_mem_64(t1, t2, t3, t4, t5, t6, t7, t8) {
	i64.store(0, t1)
	i64.store(8, t2)
	i64.store(16, t3)
	i64.store(24, t4)
	i64.store(32, t5)
	i64.store(40, t6)
	i64.store(48, t7)
	i64.store(54, t8)
}
function mload(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	let pos := u256_to_i32ptr(x1, x2, x3, x4)
	// Make room for the scratch space
	// TODO do we need to check for overflow?
	pos := i64.add(pos, 64)
	z1, z2, z3, z4 := mload_internal(pos)
}
function mload_internal(pos) -> z1, z2, z3, z4 {
	z1 := endian_swap(i64.load(pos))
	z2 := endian_swap(i64.load(i64.add(pos, 8)))
	z3 := endian_swap(i64.load(i64.add(pos, 16)))
	z4 := endian_swap(i64.load(i64.add(pos, 24)))
}
function mstore(x1, x2, x3, x4, y1, y2, y3, y4) {
	let pos := u256_to_i32ptr(x1, x2, x3, x4)
	// Make room for the scratch space
	// TODO do we need to check for overflow?
	pos := i64.add(pos, 64)
	mstore_internal(pos, y1, y2, y3, y4)
}
function mstore_internal(pos, y1, y2, y3, y4) {
	i64.store(pos, endian_swap(y1))
	i64.store(i64.add(pos, 8), endian_swap(y2))
	i64.store(i64.add(pos, 16), endian_swap(y3))
	i64.store(i64.add(pos, 24), endian_swap(y4))
}
function mstore8(x1, x2, x3, x4, y1, y2, y3, y4) {
	let pos := u256_to_i32ptr(x1, x2, x3, x4)
	// Make room for the scratch space
	// TODO do we need to check for overflow?
	pos := i64.add(pos, 64)
	let v := u256_to_byte(y1, y2, y3, y4)
	i64.store(pos, endian_swap(v))
}
// Needed?
function msize() -> z1, z2, z3, z4 {
	// TODO implement
	unreachable()
}
function sload(x1, x2, x3, x4) -> z1, z2, z3, z4 {
	mstore_internal(0, x1, x2, x3, x4)
	eth.storageLoad(0, 32)
	z1, z2, z3, z4 := mload_internal(32)
}

function sstore(x1, x2, x3, x4, y1, y2, y3, y4) {
	mstore_internal(0, x1, x2, x3, x4)
	mstore_internal(32, y1, y2, y3, y4)
	eth.storageStore(0, 32)
}

// Needed?
function pc() -> z1, z2, z3, z4 {
	// TODO implement
	unreachable()
}
function gas() -> z1, z2, z3, z4 {
	z4 := eth.getGasLeft()
}

function log0(p1, p2, p3, p4, s1, s2, s3, s4) {
	let dataOffset := u256_to_i32ptr(p1, p2, p3, p4)
	let dataLength := u256_to_i32ptr(s1, s2, s3, s4)
	eth.log(i64.add(dataOffset, 64), dataLength, 0, 0, 0, 0, 0)
}
function log1(
	p1, p2, p3, p4, s1, s2, s3, s4,
	t11, t12, t13, t14
) {
	let dataOffset := u256_to_i32ptr(p1, p2, p3, p4)
	let dataLength := u256_to_i32ptr(s1, s2, s3, s4)
	let topic1Offset := u256_to_i32ptr(t11, t12, t13, t14)
	eth.log(i64.add(dataOffset, 64), dataLength, 1, i64.add(topic1Offset, 64), 0, 0, 0)
}
function log2(
	p1, p2, p3, p4, s1, s2, s3, s4,
	t11, t12, t13, t14,
	t21, t22, t23, t24
) {
	let dataOffset := u256_to_i32ptr(p1, p2, p3, p4)
	let dataLength := u256_to_i32ptr(s1, s2, s3, s4)
	let topic1Offset := u256_to_i32ptr(t11, t12, t13, t14)
	let topic2Offset := u256_to_i32ptr(t21, t22, t23, t24)
	eth.log(i64.add(dataOffset, 64), dataLength, 2, i64.add(topic1Offset, 64), i64.add(topic2Offset, 64), 0, 0)
}
function log3(
	p1, p2, p3, p4, s1, s2, s3, s4,
	t11, t12, t13, t14,
	t21, t22, t23, t24,
	t31, t32, t33, t34
) {
	let dataOffset := u256_to_i32ptr(p1, p2, p3, p4)
	let dataLength := u256_to_i32ptr(s1, s2, s3, s4)
	let topic1Offset := u256_to_i32ptr(t11, t12, t13, t14)
	let topic2Offset := u256_to_i32ptr(t21, t22, t23, t24)
	let topic3Offset := u256_to_i32ptr(t31, t32, t33, t34)
	eth.log(i64.add(dataOffset, 64), dataLength, 3, i64.add(topic1Offset, 64), i64.add(topic2Offset, 64), i64.add(topic3Offset, 64), 0)
}
function log4(
	p1, p2, p3, p4, s1, s2, s3, s4,
	t11, t12, t13, t14,
	t21, t22, t23, t24,
	t31, t32, t33, t34,
	t41, t42, t43, t44,
) {
	let dataOffset := u256_to_i32ptr(p1, p2, p3, p4)
	let dataLength := u256_to_i32ptr(s1, s2, s3, s4)
	let topic1Offset := u256_to_i32ptr(t11, t12, t13, t14)
	let topic2Offset := u256_to_i32ptr(t21, t22, t23, t24)
	let topic3Offset := u256_to_i32ptr(t31, t32, t33, t34)
	let topic4Offset := u256_to_i32ptr(t41, t42, t43, t44)
	eth.log(i64.add(dataOffset, 64), dataLength, 4, i64.add(topic1Offset, 64), i64.add(topic2Offset, 64), i64.add(topic3Offset, 64), i64.add(topic4Offset, 64))
}

function create(
	x1, x2, x3, x4,
	y1, y2, y3, y4,
	z1, z2, z3, z4
) -> a1, a2, a3, a4 {
	let v1, v2 := u256_to_i128(x1, x2, x3, x4)
	let dataOffset := i64.add(u256_to_i32ptr(y1, y2, y3, y4), 64)
	let dataLength := u256_to_i32(z1, z2, z3, z4)
	mstore_internal(0, 0, 0, v1, v2)
	mstore_internal(32, y1, y2, y3, y4)

	let r := eth.create(0, dataOffset, dataLength, 32)
	if i64.eqz(r) {
		a1, a2, a3, a4 := mload_internal(32)
	}
	if i64.or(i64.eq(r, 1), i64.eq(r, 2)) {
		a4 := r
	}
}
function call(
	a1, a2, a3, a4,
	b1, b2, b3, b4,
	c1, c2, c3, c4,
	d1, d2, d3, d4,
	e1, e2, e3, e4,
	f1, f2, f3, f4,
	g1, g2, g3, g4
) -> x1, x2, x3, x4 {
	let dataOffest := i64.add(u256_to_i32ptr(d1, d2, d3, d4), 64)
	let dataLength := u256_to_i32(e1, e2, e3, e4)
	let g := u256_to_i64(a1, a2, a3, a4)
	let addressOffset := i64.add(dataOffest, dataLength)
	let valueOffset := i64.add(addressOffset, 32)
	mstore_internal(0, b1, b2, b3, b4)
	let v1, v2 := u256_to_i128(c1, c2, c3, c4)
	mstore_internal(32, 0, 0, v1, v2)
	x4 := eth.call(g, 0, 32, dataOffest, dataLength)
}
function callcode(
	a1, a2, a3, a4,
	b1, b2, b3, b4,
	c1, c2, c3, c4,
	d1, d2, d3, d4,
	e1, e2, e3, e4,
	f1, f2, f3, f4,
	g1, g2, g3, g4
) -> x1, x2, x3, x4 {
	let dataOffest := u256_to_i32ptr(d1, d2, d3, d4)
	let dataLength := u256_to_i32(e1, e2, e3, e4)
	let g := u256_to_i64(a1, a2, a3, a4)
	mstore_internal(0, b1, b2, b3, b4)
	let v1, v2 := u256_to_i128(c1, c2, c3, c4)
	mstore_internal(32, 0, 0, v1, v2)
	x4 := eth.callCode(g, 0, 32, dataOffest, dataLength)
}
function delegatecall(
	a1, a2, a3, a4,
	b1, b2, b3, b4,
	c1, c2, c3, c4,
	d1, d2, d3, d4,
	e1, e2, e3, e4,
	f1, f2, f3, f4
) -> x1, x2, x3, x4 {
	let dataOffest := u256_to_i32ptr(c1, c2, c3, c4)
	let dataLength := u256_to_i32(d1, d2, d3, d4)
	let g := u256_to_i64(a1, a2, a3, a4)
	mstore_internal(0, b1, b2, b3, b4)
	x4 := eth.callDelegate(g, 0, dataOffest, dataLength)
}
function staticcall(
	a1, a2, a3, a4,
	b1, b2, b3, b4,
	c1, c2, c3, c4,
	d1, d2, d3, d4,
	e1, e2, e3, e4,
	f1, f2, f3, f4
) -> x1, x2, x3, x4 {
	let gLimit := u256_to_i64(a1, a2, a3, a4)
	let dataOffset := u256_to_i32(c1, c2, c3, c4)
	let dataLength := u256_to_i32(d1, d2, d3, d4)
	mstore_internal(0, b1, b2, b3, b4)
	x4 := eth.callStatic(gLimit, 0, dataOffset, dataLength)
}
function create2(
	a1, a2, a3, a4,
	b1, b2, b3, b4,
	c1, c2, c3, c4,
	d1, d2, d3, d4
) -> x1, x2, x3, x4 {
	// TODO implement. Not implemented in ewasm yet.
	unreachable()
}
function selfdestruct(a1, a2, a3, a4) {
	mstore(0, 0, 0, 0, a1, a2, a3, a4)
	// In EVM, addresses are padded to 32 bytes, so discard the first 12.
	eth.selfDestruct(12)
}

function return(x1, x2, x3, x4, y1, y2, y3, y4) {
	eth.finish(
		// scratch - TODO: overflow check
		i64.add(u256_to_i32ptr(x1, x2, x3, x4), 64),
		u256_to_i32(y1, y2, y3, y4)
	)
}
function revert(x1, x2, x3, x4, y1, y2, y3, y4) {
	eth.revert(
		// scratch - TODO: overflow check
		i64.add(u256_to_i32ptr(x1, x2, x3, x4), 64),
		u256_to_i32(y1, y2, y3, y4)
	)
}
function invalid() {
	unreachable()
}
})"};

}

Object EVMToEWasmTranslator::run(Object const& _object)
{
	if (!m_polyfill)
		parsePolyfill();

	Block ast = boost::get<Block>(Disambiguator(m_dialect, *_object.analysisInfo)(*_object.code));
	set<YulString> reservedIdentifiers;
	NameDispenser nameDispenser{m_dialect, ast, reservedIdentifiers};
	OptimiserStepContext context{m_dialect, nameDispenser, reservedIdentifiers};

	FunctionHoister::run(context, ast);
	FunctionGrouper::run(context, ast);
	MainFunction{}(ast);
	ExpressionSplitter::run(context, ast);
	WordSizeTransform::run(m_dialect, ast, nameDispenser);

	NameDisplacer{nameDispenser, m_polyfillFunctions}(ast);
	for (auto const& st: m_polyfill->statements)
		ast.statements.emplace_back(ASTCopier{}.translate(st));

	Object ret;
	ret.name = _object.name;
	ret.code = make_shared<Block>(move(ast));
	ret.analysisInfo = make_shared<AsmAnalysisInfo>();

	ErrorList errors;
	ErrorReporter errorReporter(errors);
	AsmAnalyzer analyzer(*ret.analysisInfo, errorReporter, std::nullopt, WasmDialect::instance(), {}, _object.dataNames());
	if (!analyzer.analyze(*ret.code))
	{
		// TODO the errors here are "wrong" because they have invalid source references!
		string message;
		for (auto const& err: errors)
			message += langutil::SourceReferenceFormatter::formatErrorInformation(*err);
		yulAssert(false, message);
	}

	for (auto const& subObjectNode: _object.subObjects)
		if (Object const* subObject = dynamic_cast<Object const*>(subObjectNode.get()))
			ret.subObjects.push_back(make_shared<Object>(run(*subObject)));
		else
			ret.subObjects.push_back(make_shared<Data>(dynamic_cast<Data const&>(*subObjectNode)));
	ret.subIndexByName = _object.subIndexByName;

	return ret;
}

void EVMToEWasmTranslator::parsePolyfill()
{
	ErrorList errors;
	ErrorReporter errorReporter(errors);
	shared_ptr<Scanner> scanner{make_shared<Scanner>(CharStream(polyfill, ""))};
	m_polyfill = Parser(errorReporter, WasmDialect::instance()).parse(scanner, false);
	if (!errors.empty())
	{
		string message;
		for (auto const& err: errors)
			message += langutil::SourceReferenceFormatter::formatErrorInformation(*err);
		yulAssert(false, message);
	}

	m_polyfillFunctions.clear();
	for (auto const& statement: m_polyfill->statements)
		m_polyfillFunctions.insert(boost::get<FunctionDefinition>(statement).name);
}

