var gateway = 'ws://192.168.5.5/ws';
var websocket;
var numofelements=5;
var chartdata=[numofelements];
var counter=0;
var tempCount=5;
window.addEventListener('load',onload);

function onload(event){
  initWebSocket();
}

function initWebSocket(){
  websocket = new WebSocket(gateway);
  websocket.onopen = onOpen;
  websocket.onclose = onClose;
  websocket.onmessage = onMessage;
}

function onOpen(event) {
  console.log('Connection established');
  websocket.send("sendValues");
}

function onClose(event){
  console.log('Connection lost');
  setTimeout(initWebSocket,2000);
}

function onMessage(event){
  console.log("Recieved data");
  var myData = JSON.parse(event.data);
  var keys = Object.keys(myData);

  for(var i=0;i<keys.length;i++){
    document.getElementById(keys[i]).innerHTML = myData[keys[i]];
  }
  refreshChart(myData[keys[0]]);
  loadChart();
  if(tempCount>0){
    setTimeout(function(){
    websocket.send("sendValues");
    },3000);
    tempCount--;
  }
}

function refreshChart(parsedValue){
  if(parsedValue==0.0) return;
  if(counter<numofelements)
    chartdata[counter++] = parsedValue;
  else{
    for(var i=0;i<numofelements-1;i++)
      chartdata[i]=chartdata[i+1];
    chartdata[numofelements-1]=parsedValue;
  }
}

var svg = document.getElementById('bar-js');
var svgWidth=350 , svgHeight = 250 , barPadding = 5;
svg.setAttribute("width", svgWidth);
svg.setAttribute("height", svgHeight);

function loadChart(){
  //resetSvg so old shapes dont overflow
	svg.innerHTML="";
	var barWidth = (svgWidth / chartdata.length);
	for(var i = 0; i < chartdata.length; i++){
		//textAttribute
	  var txt = document.createElementNS("http://www.w3.org/2000/svg", "text");
		txt.textContent = chartdata[i];
		txt.setAttributeNS(null,"x",barWidth*i+barWidth/2-5);
		txt.setAttributeNS(null,"y",svgHeight - (chartdata[i]/2));
		txt.setAttributeNS(null,"dominant-baseline","middle");
		txt.setAttributeNS(null,"text-anchor","middle");
		txt.setAttributeNS(null,"font-size","40");
		txt.setAttributeNS(null,"fill","#B87333");
		//shapeAttribute
	  var rect = document.createElementNS("http://www.w3.org/2000/svg", "rect");
		rect.setAttribute("y", svgHeight - chartdata[i]);
		rect.setAttribute("height", chartdata[i]);
		rect.setAttribute("width", barWidth-barPadding);
		//combine
	  var translate = [barWidth * i, 0];
		rect.setAttribute("transform", "translate("+ translate +")");
	  svg.appendChild(rect);
	  svg.appendChild(txt);
	}
}