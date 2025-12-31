# Configuration Hash Management

1. fry-config revisa si existe un hash de configuracion localmente (guardado en un archivo)
2. fry-config envia hash de configuracion al endpoint `/device_config/sync`, si no tiene un hash, manda un string vacio ""
3. fry-config revisa la respuesta del endpoint, si la respuesta tiene HTTP status "200", no hace nada (estan en sync), si la respuesta es "201", entonces es un JSON con la configuracion actualizada
4. determinar los servicios que se deben actualizar con los hashes granulares (hash por servicio)
5. renderizar y aplicar
6. guardar hash de configuracion localmente
7. guardar hashes de servicios localmente
8. enviar un resultado de configuracion al endpoint `/device_config/reports`
