function onData(value, nodeId){
    const isOn=value & (1 << 5);
    console.log(isOn, value, nodeId);
}
