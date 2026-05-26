let base64Encode = function (str) {

    let chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let result = "";

    let i = 0;

    while (i < str.length) {

        let c1 = str.charCodeAt(i++);
        let c2 = i < str.length ? str.charCodeAt(i++) : -1;
        let c3 = i < str.length ? str.charCodeAt(i++) : -1;

        let b1 = c1 >> 2;
        let b2 = ((c1 & 3) << 4) | (c2 >= 0 ? (c2 >> 4) : 0);

        let b3;
        let b4;

        if (c2 >= 0) {
            b3 = ((c2 & 15) << 2) | (c3 >= 0 ? (c3 >> 6) : 0);
        } else {
            b3 = 64;
        }

        if (c3 >= 0) {
            b4 = c3 & 63;
        } else {
            b4 = 64;
        }

        result += chars.charAt(b1);
        result += chars.charAt(b2);

        if (b3 === 64) {
            result += "=";
        } else {
            result += chars.charAt(b3);
        }

        if (b4 === 64) {
            result += "=";
        } else {
            result += chars.charAt(b4);
        }
    }

    return result;
};

let base64Decode = function (str) {

    let chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let result = "";

    let i = 0;

    while (i < str.length) {

        let e1 = chars.indexOf(str.charAt(i++));
        let e2 = chars.indexOf(str.charAt(i++));
        let e3 = str.charAt(i++) === "=" ? -1 : chars.indexOf(str.charAt(i - 1));
        let e4 = str.charAt(i++) === "=" ? -1 : chars.indexOf(str.charAt(i - 1));

        let c1 = (e1 << 2) | (e2 >> 4);
        result += String.fromCharCode(c1);

        if (e3 >= 0) {
            let c2 = ((e2 & 15) << 4) | (e3 >> 2);
            result += String.fromCharCode(c2);
        }

        if (e4 >= 0) {
            let c3 = ((e3 & 3) << 6) | e4;
            result += String.fromCharCode(c3);
        }
    }

    return result;
};

console.log(base64Encode("hello"));
// aGVsbG8=

console.log(base64Decode("aGVsbG8="));
// hello