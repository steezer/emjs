let fib=function(n){
    if(n< 3){
        return 1;
    }
    return fib(n-1) + fib(n-2);
};

let call=function(func, a, b){
    return func(a, b);
};

console.log(12, 23, 34);
console.log(12, 23, 34);
const a = sum(12, 11);
console.log(a);
console.log(sum(12, 11));
console.log(call(sum, 1, 2))
console.log(fib(18));
try {
    const b=12;
    b=21;
} catch (error) {
    //console.log("Error:", error);
}
