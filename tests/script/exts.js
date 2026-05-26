console.log(String.fromCharCode(85, 0x4F60));

console.time('test');

const str='Abcc';
console.log(str.charCodeAt(0));
console.log(str.indexOf('C'));

console.timeLog('test');

console.log(Date.now());
console.log(Math.pow(10, 3)+Math.PI);
console.timeEnd('test');

let a={name: "srping"};

console.log('A'+a);
console.log(''+2);

let student=JSON.parse("{\"name\": \"spring\", \"age\": 16, \"gender\": false}");
console.log(student["name"].substring(1, 3));

console.log(parseInt('0x666', 10)+12);
//console.log(Date.format());

let lists=[2, 1, 3, 5, 4];
lists.push(12);
lists.sort();
console.log(lists.join('-'));           // 1-2-3-4-5-12

console.log(lists.map(x=>x*2));

// let fun2=(a, b)=>(a + b);
// console.log(lists.reduce(fun2, 0));

// lists.push({name: "demo", father: 'nihao'});
// console.log(lists);
