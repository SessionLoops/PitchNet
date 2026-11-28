// Keep the current page visible in the sidebar.
(function(){
  var cur=document.querySelector('.sidebar a.current');
  if(cur) cur.scrollIntoView({block:'center'});
})();
